/*
 * kernel/fs/minix/dir.c — MINIX v1 directory lookup and enumeration.
 *
 * Decodes fixed 16-byte MINIX v1 directory entries from cached file
 * blocks. Directory mutation is deliberately absent in chunk 1; empty
 * entries are skipped and corrupt inode numbers are logged and ignored.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "internal.h"

#include <jnu/errno.h>
#include <jnu/klog.h>
#include <jnu/minix.h>
#include <jnu/rtc.h>
#include <jnu/string.h>

#define MINIX_MODE_REG 0100000
#define MINIX_MODE_DIR 0040000

static void minix_dirent_name(const struct minix_dir_entry *de, char *name)
{
	memcpy(name, de->name, MINIX_NAME_LEN);
	name[MINIX_NAME_LEN] = '\0';
}

static int minix_clone_inode(struct vfs_inode *src, struct vfs_inode **out)
{
	struct minix_inode_info *mi = src->priv;

	return minix_inode_from_raw(src->mnt, src->ino, &mi->raw, out);
}

/*
 * Look up a child name in a MINIX directory.
 *
 * Returns:
 *   0 on success, negative errno on failure.
 */
int minix_lookup(struct vfs_inode *dir, const char *name, struct vfs_inode **out)
{
	struct minix_inode_info *mi;
	struct minix_priv *priv;
	struct minix_buffer *buf;
	uint32_t size;
	uint32_t offset;

	if (!dir->is_dir)
		return -ENOTDIR;

	mi = dir->priv;
	priv = dir->mnt->priv;
	size = mi->raw.i_size;
	offset = 0;

	if (strcmp(name, ".") == 0)
		return minix_clone_inode(dir, out);

	while (offset < size) {
		uint32_t b = minix_bmap(dir->mnt, mi,
					offset / MINIX_BLOCK_SIZE, false);
		uint32_t chunk;

		if (b == 0) {
			offset += MINIX_BLOCK_SIZE;
			continue;
		}

		buf = bufcache_get(dir->mnt->bdev, b);
		if (!buf)
			return -EIO;

		chunk = size - offset;
		if (chunk > MINIX_BLOCK_SIZE)
			chunk = MINIX_BLOCK_SIZE;

		for (uint32_t i = 0; i < chunk; i += sizeof(struct minix_dir_entry)) {
			struct minix_dir_entry *de =
				(struct minix_dir_entry *)(buf->data + i);
			struct minix_raw_inode raw;
			struct vfs_inode *inode;
			char dename[MINIX_NAME_LEN + 1];
			int err;

			if (de->inode == 0)
				continue;

			if (de->inode > priv->sb.s_ninodes) {
				pr_err("minix: malicious inode number %u in "
				       "directory\n",
				       de->inode);
				continue;
			}

			minix_dirent_name(de, dename);
			if (strcmp(dename, name) != 0)
				continue;

			err = minix_get_raw_inode(dir->mnt, de->inode, &raw);
			if (err) {
				bufcache_put(buf);
				return -EIO;
			}

			err = minix_inode_from_raw(dir->mnt, de->inode, &raw,
						   &inode);
			if (err) {
				bufcache_put(buf);
				return err;
			}

			bufcache_put(buf);
			*out = inode;
			return 0;
		}

		bufcache_put(buf);
		offset += MINIX_BLOCK_SIZE;
	}

	return -ENOENT;
}

/*
 * Return the Nth live directory entry.
 *
 * Returns 1 when an entry is found, 0 at EOF, or negative errno.
 */
int minix_readdir(struct vfs_inode *dir, size_t index, struct vfs_dirent *out)
{
	struct minix_inode_info *mi;
	struct minix_priv *priv;
	size_t curr_idx = 0;
	uint32_t size;
	uint32_t offset;

	if (!dir->is_dir)
		return -ENOTDIR;

	mi = dir->priv;
	priv = dir->mnt->priv;
	size = mi->raw.i_size;
	offset = 0;

	while (offset < size) {
		uint32_t b = minix_bmap(dir->mnt, mi,
					offset / MINIX_BLOCK_SIZE, false);
		struct minix_buffer *buf;
		uint32_t chunk;

		if (b == 0) {
			offset += MINIX_BLOCK_SIZE;
			continue;
		}

		buf = bufcache_get(dir->mnt->bdev, b);
		if (!buf)
			return -EIO;

		chunk = size - offset;
		if (chunk > MINIX_BLOCK_SIZE)
			chunk = MINIX_BLOCK_SIZE;

		for (uint32_t i = 0; i < chunk; i += sizeof(struct minix_dir_entry)) {
			struct minix_dir_entry *de =
				(struct minix_dir_entry *)(buf->data + i);

			if (de->inode == 0)
				continue;

			if (de->inode > priv->sb.s_ninodes) {
				pr_err("minix: malicious inode number %u in "
				       "directory\n",
				       de->inode);
				continue;
			}

			if (curr_idx == index) {
				out->ino = de->inode;
				memcpy(out->name, de->name, MINIX_NAME_LEN);
				out->name[MINIX_NAME_LEN] = '\0';
				bufcache_put(buf);
				return 1;
			}
			curr_idx++;
		}

		bufcache_put(buf);
		offset += MINIX_BLOCK_SIZE;
	}

	return 0;
}

static int minix_find_dirent(struct vfs_inode *dir, const char *name,
			     uint32_t *entry_off, struct minix_dir_entry *out)
{
	struct minix_inode_info *mi = dir->priv;
	uint32_t offset = 0;

	while (offset < mi->raw.i_size) {
		uint32_t b = minix_bmap(dir->mnt, mi,
					offset / MINIX_BLOCK_SIZE, false);
		struct minix_buffer *buf;
		uint32_t chunk;

		if (b == 0) {
			offset += MINIX_BLOCK_SIZE;
			continue;
		}
		buf = bufcache_get(dir->mnt->bdev, b);
		if (!buf)
			return -EIO;
		chunk = mi->raw.i_size - offset;
		if (chunk > MINIX_BLOCK_SIZE)
			chunk = MINIX_BLOCK_SIZE;

		for (uint32_t i = 0; i < chunk; i += sizeof(struct minix_dir_entry)) {
			struct minix_dir_entry *de =
				(struct minix_dir_entry *)(buf->data + i);
			char dename[MINIX_NAME_LEN + 1];

			if (de->inode == 0)
				continue;
			minix_dirent_name(de, dename);
			if (strcmp(dename, name) == 0) {
				if (entry_off)
					*entry_off = offset + i;
				if (out)
					memcpy(out, de, sizeof(*out));
				bufcache_put(buf);
				return 0;
			}
		}
		bufcache_put(buf);
		offset += MINIX_BLOCK_SIZE;
	}

	return -ENOENT;
}

static int minix_write_dirent_at(struct vfs_inode *dir, uint32_t offset,
				 const struct minix_dir_entry *de)
{
	ssize_t n;

	n = minix_write(dir, offset, sizeof(*de), de);
	if (n < 0)
		return (int)n;
	return n == (ssize_t)sizeof(*de) ? 0 : -EIO;
}

static int minix_add_dirent(struct vfs_inode *dir, const char *name,
			    uint32_t ino)
{
	struct minix_inode_info *mi = dir->priv;
	struct minix_dir_entry de;
	uint32_t offset = 0;

	if (strlen(name) > MINIX_NAME_LEN)
		return -ENAMETOOLONG;
	if (minix_find_dirent(dir, name, NULL, NULL) == 0)
		return -EEXIST;

	memset(&de, 0, sizeof(de));
	de.inode = (uint16_t)ino;
	memcpy(de.name, name, strlen(name));

	while (offset < mi->raw.i_size) {
		uint32_t b = minix_bmap(dir->mnt, mi,
					offset / MINIX_BLOCK_SIZE, false);
		struct minix_buffer *buf;
		uint32_t chunk;

		if (b == 0) {
			offset += MINIX_BLOCK_SIZE;
			continue;
		}
		buf = bufcache_get(dir->mnt->bdev, b);
		if (!buf)
			return -EIO;
		chunk = mi->raw.i_size - offset;
		if (chunk > MINIX_BLOCK_SIZE)
			chunk = MINIX_BLOCK_SIZE;
		for (uint32_t i = 0; i < chunk; i += sizeof(struct minix_dir_entry)) {
			struct minix_dir_entry *slot =
				(struct minix_dir_entry *)(buf->data + i);

			if (slot->inode == 0) {
				bufcache_put(buf);
				return minix_write_dirent_at(dir, offset + i,
							     &de);
			}
		}
		bufcache_put(buf);
		offset += MINIX_BLOCK_SIZE;
	}

	return minix_write_dirent_at(dir, mi->raw.i_size, &de);
}

static int minix_zero_dirent(struct vfs_inode *dir, const char *name,
			     struct minix_dir_entry *old)
{
	struct minix_dir_entry zero;
	uint32_t off;
	int err;

	err = minix_find_dirent(dir, name, &off, old);
	if (err)
		return err;
	memset(&zero, 0, sizeof(zero));
	return minix_write_dirent_at(dir, off, &zero);
}

static int minix_restore_dirent(struct vfs_inode *dir, const char *name,
				const struct minix_dir_entry *de)
{
	struct minix_dir_entry existing;

	if (minix_find_dirent(dir, name, NULL, &existing) == 0)
		return -EEXIST;
	return minix_add_dirent(dir, name, de->inode);
}

static bool minix_dir_empty(struct vfs_inode *dir)
{
	struct vfs_dirent dent;

	for (size_t i = 0; minix_readdir(dir, i, &dent) == 1; i++) {
		if (strcmp(dent.name, ".") != 0 && strcmp(dent.name, "..") != 0)
			return false;
	}
	return true;
}

static int minix_replace_dotdot(struct vfs_inode *dir, uint32_t parent)
{
	struct minix_dir_entry de;
	uint32_t off;
	int err;

	err = minix_find_dirent(dir, "..", &off, &de);
	if (err)
		return err;
	de.inode = (uint16_t)parent;
	return minix_write_dirent_at(dir, off, &de);
}

static int minix_free_tree_inode(struct vfs_inode *parent, uint32_t ino,
				 const struct minix_raw_inode *raw)
{
	struct vfs_inode *victim;
	int err;

	err = minix_inode_from_raw(parent->mnt, ino, raw, &victim);
	if (err)
		return err;
	if (victim->is_dir && !minix_dir_empty(victim)) {
		vfs_close(victim);
		return -EBUSY;
	}
	err = minix_truncate(victim, 0);
	vfs_close(victim);
	if (err)
		return err;
	minix_free_inode(parent->mnt, ino);
	return 0;
}

int minix_create(struct vfs_inode *dir, const char *name, uint16_t mode,
		 struct vfs_inode **out)
{
	struct minix_inode_info mi;
	uint32_t ino;
	int err;

	if (!dir->is_dir)
		return -ENOTDIR;
	ino = minix_alloc_inode(dir->mnt);
	if (ino == 0)
		return -ENOSPC;

	memset(&mi, 0, sizeof(mi));
	mi.raw.i_mode = (uint16_t)(MINIX_MODE_REG | (mode & 0777));
	mi.raw.i_time = rtc_now_unix();
	mi.raw.i_nlinks = 1;
	mi.dirty = true;
	err = minix_write_inode(dir->mnt, ino, &mi);
	if (err)
		goto fail_inode;
	err = minix_add_dirent(dir, name, ino);
	if (err)
		goto fail_inode;
	return minix_inode_from_raw(dir->mnt, ino, &mi.raw, out);

fail_inode:
	minix_free_inode(dir->mnt, ino);
	return err;
}

int minix_unlink(struct vfs_inode *dir, const char *name)
{
	struct minix_dir_entry de;
	struct minix_raw_inode raw;
	int err;

	err = minix_find_dirent(dir, name, NULL, &de);
	if (err)
		return err;
	err = minix_get_raw_inode(dir->mnt, de.inode, &raw);
	if (err)
		return err;
	if ((raw.i_mode & MINIX_MODE_DIR) != 0)
		return -EISDIR;

	err = minix_zero_dirent(dir, name, &de);
	if (err)
		return err;
	err = minix_free_tree_inode(dir, de.inode, &raw);
	if (err) {
		(void)minix_restore_dirent(dir, name, &de);
		return err;
	}
	return 0;
}

int minix_mkdir(struct vfs_inode *dir, const char *name, uint16_t mode)
{
	struct minix_inode_info mi;
	struct vfs_inode tmp;
	uint32_t ino;
	int err;

	ino = minix_alloc_inode(dir->mnt);
	if (ino == 0)
		return -ENOSPC;
	memset(&mi, 0, sizeof(mi));
	mi.raw.i_mode = (uint16_t)(MINIX_MODE_DIR | (mode & 0777));
	mi.raw.i_time = rtc_now_unix();
	mi.raw.i_nlinks = 2;
	mi.dirty = true;

	memset(&tmp, 0, sizeof(tmp));
	tmp.mnt = dir->mnt;
	tmp.ino = ino;
	tmp.size = 0;
	tmp.is_dir = true;
	tmp.mode = mi.raw.i_mode;
	tmp.priv = &mi;

	err = minix_add_dirent(&tmp, ".", ino);
	if (err)
		goto fail_inode;
	err = minix_add_dirent(&tmp, "..", dir->ino);
	if (err)
		goto fail_inode;
	err = minix_write_inode(dir->mnt, ino, &mi);
	if (err)
		goto fail_inode;
	err = minix_add_dirent(dir, name, ino);
	if (err)
		goto fail_inode;
	{
		struct minix_inode_info *parent = dir->priv;

		parent->raw.i_nlinks++;
		parent->dirty = true;
	}
	return 0;

fail_inode:
	(void)minix_truncate(&tmp, 0);
	minix_free_inode(dir->mnt, ino);
	return err;
}

int minix_rmdir(struct vfs_inode *dir, const char *name)
{
	struct minix_dir_entry de;
	struct vfs_inode *victim;
	struct minix_inode_info *parent = dir->priv;
	int err;

	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		return -EINVAL;
	err = minix_lookup(dir, name, &victim);
	if (err)
		return err;
	if (!victim->is_dir) {
		vfs_close(victim);
		return -ENOTDIR;
	}
	if (!minix_dir_empty(victim)) {
		vfs_close(victim);
		return -EBUSY;
	}
	err = minix_truncate(victim, 0);
	vfs_close(victim);
	if (err)
		return err;
	err = minix_zero_dirent(dir, name, &de);
	if (err)
		return err;
	minix_free_inode(dir->mnt, de.inode);
	if (parent->raw.i_nlinks > 0)
		parent->raw.i_nlinks--;
	parent->dirty = true;
	return 0;
}

int minix_rename(struct vfs_inode *old_dir, const char *old_name,
		 struct vfs_inode *new_dir, const char *new_name)
{
	struct minix_dir_entry de;
	struct minix_dir_entry replaced;
	struct minix_raw_inode old_raw;
	struct minix_raw_inode replaced_raw;
	struct vfs_inode *old_inode;
	bool had_replaced = false;
	int err;

	if (old_dir == new_dir && strcmp(old_name, new_name) == 0)
		return 0;
	if (strcmp(old_name, ".") == 0 || strcmp(old_name, "..") == 0 ||
	    strcmp(new_name, ".") == 0 || strcmp(new_name, "..") == 0)
		return -EINVAL;

	err = minix_find_dirent(old_dir, old_name, NULL, &de);
	if (err)
		return err;
	err = minix_get_raw_inode(old_dir->mnt, de.inode, &old_raw);
	if (err)
		return err;
	err = minix_inode_from_raw(old_dir->mnt, de.inode, &old_raw, &old_inode);
	if (err)
		return err;

	err = minix_find_dirent(new_dir, new_name, NULL, &replaced);
	if (err == 0) {
		had_replaced = true;
		err = minix_get_raw_inode(new_dir->mnt, replaced.inode,
					  &replaced_raw);
		if (err)
			goto out_old_inode;
		if (old_inode->is_dir && (replaced_raw.i_mode & MINIX_MODE_DIR) == 0) {
			err = -ENOTDIR;
			goto out_old_inode;
		}
		if (!old_inode->is_dir && (replaced_raw.i_mode & MINIX_MODE_DIR) != 0) {
			err = -EISDIR;
			goto out_old_inode;
		}
		if ((replaced_raw.i_mode & MINIX_MODE_DIR) != 0) {
			struct vfs_inode *dst_dir;

			err = minix_inode_from_raw(new_dir->mnt, replaced.inode,
						   &replaced_raw, &dst_dir);
			if (err)
				goto out_old_inode;
			if (!minix_dir_empty(dst_dir))
				err = -EBUSY;
			vfs_close(dst_dir);
			if (err)
				goto out_old_inode;
		}
		err = minix_zero_dirent(new_dir, new_name, &replaced);
		if (err)
			goto out_old_inode;
	} else if (err != -ENOENT) {
		goto out_old_inode;
	}

	err = minix_add_dirent(new_dir, new_name, de.inode);
	if (err) {
		if (had_replaced)
			(void)minix_restore_dirent(new_dir, new_name, &replaced);
		goto out_old_inode;
	}
	err = minix_zero_dirent(old_dir, old_name, &de);
	if (err) {
		(void)minix_zero_dirent(new_dir, new_name, NULL);
		if (had_replaced)
			(void)minix_restore_dirent(new_dir, new_name, &replaced);
		goto out_old_inode;
	}

	if (old_inode->is_dir && old_dir != new_dir) {
		struct minix_inode_info *old_parent = old_dir->priv;
		struct minix_inode_info *new_parent = new_dir->priv;

		err = minix_replace_dotdot(old_inode, new_dir->ino);
		if (err)
			goto out_old_inode;
		if (old_parent->raw.i_nlinks > 0)
			old_parent->raw.i_nlinks--;
		new_parent->raw.i_nlinks++;
		old_parent->dirty = true;
		new_parent->dirty = true;
	}
	if (had_replaced) {
		err = minix_free_tree_inode(new_dir, replaced.inode, &replaced_raw);
		if (err)
			goto out_old_inode;
	}

out_old_inode:
	vfs_close(old_inode);
	return err;
}

int minix_dir_selftest(void)
{
	struct vfs_inode *ino;
	int err;

	(void)vfs_unlink("/c2-dir/moved");
	(void)vfs_unlink("/c2-ren");
	(void)vfs_rmdir("/c2-dir");

	err = vfs_mkdir("/c2-dir", 0777);
	if (err) {
		pr_err("minix_dir_selftest: mkdir failed err=%d\n", err);
		return err;
	}
	err = vfs_create("/c2-ren", 0666, &ino);
	if (err) {
		pr_err("minix_dir_selftest: create failed err=%d\n", err);
		return err;
	}
	vfs_close(ino);
	err = vfs_rename("/c2-ren", "/c2-dir/moved");
	if (err) {
		pr_err("minix_dir_selftest: rename failed err=%d\n", err);
		return err;
	}
	err = vfs_open("/c2-dir/moved", &ino);
	if (err) {
		pr_err("minix_dir_selftest: open moved failed err=%d\n", err);
		return err;
	}
	vfs_close(ino);
	err = vfs_unlink("/c2-dir/moved");
	if (err)
		return err;
	err = vfs_rmdir("/c2-dir");
	if (err)
		return err;

	pr_info("minix_dir_selftest: [ OK ]\n");
	return 0;
}
