/*
 * kernel/fs/fat32/file.c — FAT32 regular-file reads.
 *
 * Translates a byte offset into a cluster within the file's chain, then
 * into a sector LBA, and copies through the FAT32 sector cache. No
 * filesystem code issues bare block-device I/O. The backend is
 * read-only in v0.0.4.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "internal.h"

#include <jnu/lib/string.h>
#include <uapi/jnu/errno.h>

/*
 * Read bytes from a FAT32 file.
 *
 * Returns the number of bytes read on success (possibly short at EOF or
 * if the chain ends early on a corrupt image), or negative errno.
 */
ssize_t fat32_read(struct vfs_inode *ino, uint64_t offset, size_t len,
		   void *buf)
{
	struct fat32_priv *priv = ino->mnt->priv;
	struct fat32_inode_info *fi = ino->priv;
	uint8_t *p = buf;
	uint32_t cluster;
	uint64_t cluster_index;
	uint32_t guard = 0;
	size_t left;
	uint64_t cur;

	if (ino->is_dir)
		return -EISDIR;
	if (offset >= ino->size)
		return 0;
	if (len > (size_t)(ino->size - offset))
		len = (size_t)(ino->size - offset);
	if (len == 0)
		return 0;

	/* Walk the chain to the cluster that holds `offset`. */
	cluster = fi->first_cluster;
	cluster_index = offset / priv->cluster_bytes;
	for (uint64_t i = 0; i < cluster_index; i++) {
		if (!fat32_cluster_valid(priv, cluster))
			return 0;
		cluster = fat32_next_cluster(ino->mnt, cluster);
		if (fat32_is_eoc(cluster) || ++guard > priv->cluster_count)
			return 0;
	}

	left = len;
	cur = offset;
	while (left > 0) {
		uint32_t coff = (uint32_t)(cur % priv->cluster_bytes);
		uint32_t sec_in_clus = coff / priv->bytes_per_sec;
		uint32_t soff = coff % priv->bytes_per_sec;
		struct fat32_buffer *b;
		uint64_t lba;
		uint32_t chunk;

		if (!fat32_cluster_valid(priv, cluster))
			break;

		lba = fat32_cluster_lba(priv, cluster) + sec_in_clus;
		b = fat32_bget(ino->mnt->bdev, lba);
		if (!b)
			return -EIO;

		chunk = priv->bytes_per_sec - soff;
		if (chunk > left)
			chunk = (uint32_t)left;
		memcpy(p, b->data + soff, chunk);
		fat32_bput(b);

		p += chunk;
		left -= chunk;
		cur += chunk;

		/* Crossed a cluster boundary? Follow the chain. */
		if (left > 0 && (cur % priv->cluster_bytes) == 0) {
			cluster = fat32_next_cluster(ino->mnt, cluster);
			if (fat32_is_eoc(cluster) ||
			    ++guard > priv->cluster_count)
				break;
		}
	}

	return (ssize_t)(len - left);
}
