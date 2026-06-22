/*
 * kernel/fs/fat32/dir.c — FAT32 directory lookup and enumeration.
 *
 * Decodes fixed 32-byte FAT directory entries from the directory's
 * cluster chain. Free slots (0xe5), the end marker (0x00), volume-label
 * entries, and long-name slots are skipped; only 8.3 short names are
 * returned.
 *
 * TODO(LFN): Long File Name support is deliberately not implemented in
 * v0.0.4. LFN slots (attr == FAT32_ATTR_LONG_NAME) are skipped here, so
 * a file created with only a long name is currently invisible. Adding
 * LFN means reassembling the UCS-2 name from the preceding LFN slots in
 * this file AND widening VFS_NAME_MAX in include/jnu/fs/vfs.h (currently
 * 64; LFN allows up to 255), which gates the feature.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "internal.h"

#include <jnu/lib/klog.h>
#include <jnu/lib/string.h>
#include <uapi/jnu/errno.h>

/*
 * Decode a packed 8.3 name into "BASE.EXT" form. Trailing spaces are
 * trimmed from both the base and extension; the '.' is omitted when
 * there is no extension. The escaped 0x05 first byte (a real 0xe5) is
 * restored. Output is at most 12 chars plus the terminating NUL.
 */
void fat32_decode_83(const uint8_t name[FAT32_NAME_LEN], char *out)
{
	int n = 0;
	int base_len = 8;
	int ext_len = 3;

	while (base_len > 0 && name[base_len - 1] == ' ')
		base_len--;
	for (int i = 0; i < base_len; i++) {
		uint8_t c = name[i];

		if (i == 0 && c == FAT32_DIRENT_E5)
			c = FAT32_DIRENT_FREE;
		out[n++] = (char)c;
	}

	while (ext_len > 0 && name[8 + ext_len - 1] == ' ')
		ext_len--;
	if (ext_len > 0) {
		out[n++] = '.';
		for (int i = 0; i < ext_len; i++)
			out[n++] = (char)name[8 + i];
	}

	out[n] = '\0';
}

/* Case-insensitive ASCII comparison (FAT short names are case-folded). */
static bool fat32_name_eq(const char *a, const char *b)
{
	for (;;) {
		char ca = *a++;
		char cb = *b++;

		if (ca >= 'a' && ca <= 'z')
			ca = (char)(ca - 32);
		if (cb >= 'a' && cb <= 'z')
			cb = (char)(cb - 32);
		if (ca != cb)
			return false;
		if (ca == '\0')
			return true;
	}
}

/*
 * Synthesize a stable inode number from a directory entry's on-disk
 * location: 16 entries per 512 B sector, so (lba * 16 + slot) is unique
 * per directory slot and dense enough to fit 32 bits for the disk image
 * sizes JNU mounts. Identity option (b) from the design debate.
 *
 * TODO(c): for multi-TB media this can overflow 32 bits; a per-mount
 * handle table keyed by on-disk location would lift the ceiling. Not
 * needed today.
 */
static uint32_t fat32_dirent_ino(uint64_t lba, uint32_t off_in_sector)
{
	return (uint32_t)(lba * (FAT32_SECTOR_SIZE / FAT32_DIRENT_SIZE) +
			  off_in_sector / FAT32_DIRENT_SIZE);
}

/*
 * Reject directory entries whose packed 8.3 name contains bytes that are
 * illegal in a short name. A malicious or corrupt image could otherwise
 * smuggle control characters, an embedded NUL, or a path separator ('/')
 * through fat32_decode_83() into a name handed back to the VFS/userspace.
 * The only entries that legitimately carry '.' are "." and ".." (which
 * also begin with '.'), so a leading dot is accepted wholesale.
 */
static bool fat32_name_valid(const uint8_t name[FAT32_NAME_LEN])
{
	if (name[0] == '.')
		return true;

	for (int i = 0; i < FAT32_NAME_LEN; i++) {
		uint8_t c = name[i];

		/* A leading 0x05 is the escape for a real 0xe5 byte. */
		if (i == 0 && c == FAT32_DIRENT_E5)
			c = FAT32_DIRENT_FREE;
		if (c < 0x20) /* control chars and embedded NUL */
			return false;
		switch (c) {
		case '"':
		case '*':
		case '+':
		case ',':
		case '.':
		case '/':
		case ':':
		case ';':
		case '<':
		case '=':
		case '>':
		case '?':
		case '[':
		case '\\':
		case ']':
		case '|':
			return false;
		default:
			break;
		}
	}
	return true;
}

/* True for entries that should be skipped during a directory scan. */
static bool fat32_entry_skip(const struct fat32_dirent *de)
{
	if (de->name[0] == FAT32_DIRENT_FREE)
		return true;
	if ((de->attr & FAT32_ATTR_LONG_NAME) == FAT32_ATTR_LONG_NAME)
		return true;
	if (de->attr & FAT32_ATTR_VOLUME_ID)
		return true;
	if (!fat32_name_valid(de->name))
		return true;
	return false;
}

/*
 * Look up a child name in a FAT32 directory.
 *
 * Returns 0 on success, negative errno on failure.
 */
int fat32_lookup(struct vfs_inode *dir, const char *name,
		 struct vfs_inode **out)
{
	struct fat32_priv *priv;
	struct fat32_inode_info *di;
	uint32_t cluster;
	uint32_t guard = 0;

	if (!dir->is_dir)
		return -ENOTDIR;
	if (strcmp(name, ".") == 0)
		return fat32_clone_inode(dir, out);

	priv = dir->mnt->priv;
	di = dir->priv;
	cluster = di->first_cluster;

	while (fat32_cluster_valid(priv, cluster)) {
		uint64_t base = fat32_cluster_lba(priv, cluster);

		if (guard++ > priv->cluster_count) {
			pr_err("fat32: directory cluster chain loop\n");
			return -EIO;
		}

		for (uint32_t s = 0; s < priv->sec_per_clus; s++) {
			uint64_t lba = base + s;
			struct fat32_buffer *buf;

			buf = fat32_bget(dir->mnt->bdev, lba);
			if (!buf)
				return -EIO;

			for (uint32_t o = 0; o < priv->bytes_per_sec;
			     o += FAT32_DIRENT_SIZE) {
				struct fat32_dirent *de =
				    (struct fat32_dirent *)(buf->data + o);
				char dename[16];
				struct fat32_dirent copy;
				uint32_t ino;
				int err;

				if (de->name[0] == FAT32_DIRENT_END) {
					fat32_bput(buf);
					return -ENOENT;
				}
				if (fat32_entry_skip(de))
					continue;

				fat32_decode_83(de->name, dename);
				if (!fat32_name_eq(dename, name))
					continue;

				ino = fat32_dirent_ino(lba, o);
				copy = *de;
				fat32_bput(buf);
				err = fat32_inode_build(dir->mnt, ino, &copy,
							out);
				return err;
			}
			fat32_bput(buf);
		}

		cluster = fat32_next_cluster(dir->mnt, cluster);
		if (fat32_is_eoc(cluster))
			break;
	}

	return -ENOENT;
}

/*
 * Return the Nth live directory entry.
 *
 * Returns 1 when an entry is found, 0 at end of directory, or negative
 * errno on error.
 */
int fat32_readdir(struct vfs_inode *dir, size_t index, struct vfs_dirent *out)
{
	struct fat32_priv *priv;
	struct fat32_inode_info *di;
	uint32_t cluster;
	uint32_t guard = 0;
	size_t cur = 0;

	if (!dir->is_dir)
		return -ENOTDIR;

	priv = dir->mnt->priv;
	di = dir->priv;
	cluster = di->first_cluster;

	while (fat32_cluster_valid(priv, cluster)) {
		uint64_t base = fat32_cluster_lba(priv, cluster);

		if (guard++ > priv->cluster_count) {
			pr_err("fat32: directory cluster chain loop\n");
			return -EIO;
		}

		for (uint32_t s = 0; s < priv->sec_per_clus; s++) {
			uint64_t lba = base + s;
			struct fat32_buffer *buf;

			buf = fat32_bget(dir->mnt->bdev, lba);
			if (!buf)
				return -EIO;

			for (uint32_t o = 0; o < priv->bytes_per_sec;
			     o += FAT32_DIRENT_SIZE) {
				struct fat32_dirent *de =
				    (struct fat32_dirent *)(buf->data + o);

				if (de->name[0] == FAT32_DIRENT_END) {
					fat32_bput(buf);
					return 0;
				}
				if (fat32_entry_skip(de))
					continue;

				if (cur == index) {
					out->ino = fat32_dirent_ino(lba, o);
					fat32_decode_83(de->name, out->name);
					fat32_bput(buf);
					return 1;
				}
				cur++;
			}
			fat32_bput(buf);
		}

		cluster = fat32_next_cluster(dir->mnt, cluster);
		if (fat32_is_eoc(cluster))
			break;
	}

	return 0;
}
