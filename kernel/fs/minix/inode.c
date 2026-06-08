/*
 * kernel/fs/minix/inode.c — MINIX v1 inode and block mapping helpers.
 *
 * Reads packed on-disk inodes through the buffer cache and resolves
 * direct, single-indirect, and double-indirect block pointers. The code
 * remains read-only in chunk 1 and rejects block pointers outside the
 * mounted filesystem's data zone.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "internal.h"

#include <jnu/lib/klog.h>
#include <jnu/lib/mutex.h>
#include <jnu/lib/string.h>
#include <jnu/mm/kmalloc.h>
#include <uapi/jnu/errno.h>

/*
 * Read one raw MINIX inode from the inode table.
 *
 * Parameters:
 *   mnt  mounted MINIX filesystem.
 *   ino  MINIX inode number, starting at 1.
 *   out  destination for the packed on-disk inode.
 *
 * Returns:
 *   0 on success, negative errno on failure.
 */
int minix_get_raw_inode(struct vfs_mount *mnt, uint32_t ino,
			struct minix_raw_inode *out)
{
	struct minix_priv *priv = mnt->priv;
	struct minix_buffer *buf;
	uint32_t block;
	uint32_t offset;

	if (ino == 0)
		return -EINVAL;
	if (ino > priv->sb.s_ninodes)
		return -EINVAL;

	block = priv->inodes_start_block +
		(ino - 1) / (MINIX_BLOCK_SIZE / sizeof(struct minix_raw_inode));
	offset =
	    ((ino - 1) % (MINIX_BLOCK_SIZE / sizeof(struct minix_raw_inode))) *
	    sizeof(struct minix_raw_inode);

	buf = bufcache_get(mnt->bdev, block);
	if (!buf)
		return -EIO;

	memcpy(out, buf->data + offset, sizeof(*out));
	bufcache_put(buf);
	return 0;
}

/*
 * Resolve a file-relative logical block to an on-disk MINIX zone.
 *
 * Returns 0 for holes, missing indirect tables, unreadable indirect
 * blocks, or invalid/corrupt pointers.
 */
static uint32_t minix_alloc_data_zone(struct vfs_mount *mnt)
{
	uint32_t zone = minix_alloc_zone(mnt);
	struct minix_buffer *buf;

	if (zone == 0)
		return 0;

	buf = bufcache_get(mnt->bdev, zone);
	if (!buf) {
		minix_free_zone(mnt, zone);
		return 0;
	}
	memset(buf->data, 0, MINIX_BLOCK_SIZE);
	bufcache_mark_dirty(buf);
	bufcache_put(buf);
	return zone;
}

uint32_t minix_bmap(struct vfs_mount *mnt, struct minix_inode_info *mi,
		    uint32_t block, bool create)
{
	struct minix_priv *priv = mnt->priv;
	uint16_t per_block = MINIX_BLOCK_SIZE / sizeof(uint16_t);
	struct minix_buffer *buf;
	struct minix_raw_inode *ino = &mi->raw;
	uint32_t ind1;
	uint32_t b = 0;

	if (block < 7) {
		if (ino->i_zone[block] == 0 && create) {
			ino->i_zone[block] =
			    (uint16_t)minix_alloc_data_zone(mnt);
			mi->dirty = true;
		}
		b = ino->i_zone[block];
		goto check;
	}
	block -= 7;

	if (block < per_block) {
		if (ino->i_zone[7] == 0 && create) {
			ino->i_zone[7] = (uint16_t)minix_alloc_data_zone(mnt);
			mi->dirty = true;
		}
		if (ino->i_zone[7] == 0)
			return 0;
		if (ino->i_zone[7] < priv->sb.s_firstdatazone ||
		    ino->i_zone[7] >= priv->sb.s_nzones) {
			pr_err(
			    "minix: out of bounds single indirect pointer %u\n",
			    ino->i_zone[7]);
			return 0;
		}
		buf = bufcache_get(mnt->bdev, ino->i_zone[7]);
		if (!buf)
			return 0;
		if (((uint16_t *)buf->data)[block] == 0 && create) {
			((uint16_t *)buf->data)[block] =
			    (uint16_t)minix_alloc_data_zone(mnt);
			bufcache_mark_dirty(buf);
		}
		b = ((uint16_t *)buf->data)[block];
		bufcache_put(buf);
		goto check;
	}
	block -= per_block;

	if (block < (uint32_t)per_block * per_block) {
		if (ino->i_zone[8] == 0 && create) {
			ino->i_zone[8] = (uint16_t)minix_alloc_data_zone(mnt);
			mi->dirty = true;
		}
		if (ino->i_zone[8] == 0)
			return 0;
		if (ino->i_zone[8] < priv->sb.s_firstdatazone ||
		    ino->i_zone[8] >= priv->sb.s_nzones) {
			pr_err(
			    "minix: out of bounds double indirect pointer %u\n",
			    ino->i_zone[8]);
			return 0;
		}
		buf = bufcache_get(mnt->bdev, ino->i_zone[8]);
		if (!buf)
			return 0;
		if (((uint16_t *)buf->data)[block / per_block] == 0 && create) {
			((uint16_t *)buf->data)[block / per_block] =
			    (uint16_t)minix_alloc_data_zone(mnt);
			bufcache_mark_dirty(buf);
		}
		ind1 = ((uint16_t *)buf->data)[block / per_block];
		bufcache_put(buf);

		if (ind1 == 0)
			return 0;
		if (ind1 < priv->sb.s_firstdatazone ||
		    ind1 >= priv->sb.s_nzones) {
			pr_err("minix: out of bounds single indirect pointer "
			       "in double indirect %u\n",
			       ind1);
			return 0;
		}
		buf = bufcache_get(mnt->bdev, ind1);
		if (!buf)
			return 0;
		b = ((uint16_t *)buf->data)[block % per_block];
		bufcache_put(buf);
		goto check;
	}

check:
	/*
	 * Reject block pointers that fall outside the data zone. A
	 * malicious or corrupt image can otherwise point a regular file
	 * at the inode table, the inode bitmap, or the zone bitmap, and
	 * a userspace read() of that file would receive raw FS metadata.
	 */
	if (b != 0 &&
	    (b < priv->sb.s_firstdatazone || b >= priv->sb.s_nzones)) {
		pr_err("minix: out of bounds block pointer %u "
		       "(firstdatazone=%u, nzones=%u)\n",
		       b, priv->sb.s_firstdatazone, priv->sb.s_nzones);
		return 0;
	}
	return b;
}

int minix_write_inode(struct vfs_mount *mnt, uint32_t ino,
		      struct minix_inode_info *mi)
{
	struct minix_priv *priv = mnt->priv;
	struct minix_buffer *buf;
	uint32_t block;
	uint32_t offset;

	if (!mi->dirty)
		return 0;
	if (ino == 0 || ino > priv->sb.s_ninodes)
		return -EINVAL;

	block = priv->inodes_start_block +
		(ino - 1) / (MINIX_BLOCK_SIZE / sizeof(struct minix_raw_inode));
	offset =
	    ((ino - 1) % (MINIX_BLOCK_SIZE / sizeof(struct minix_raw_inode))) *
	    sizeof(struct minix_raw_inode);

	buf = bufcache_get(mnt->bdev, block);
	if (!buf)
		return -EIO;

	memcpy(buf->data + offset, &mi->raw, sizeof(mi->raw));
	bufcache_mark_dirty(buf);
	bufcache_put(buf);
	mi->dirty = false;

	if (mnt->root && mnt->root->ino == ino && mnt->root->priv != mi) {
		struct minix_inode_info *root_mi = mnt->root->priv;

		memcpy(&root_mi->raw, &mi->raw, sizeof(mi->raw));
		root_mi->dirty = false;
		mnt->root->size = mi->raw.i_size;
		mnt->root->is_dir = (mi->raw.i_mode & 040000) != 0;
		mnt->root->mode = mi->raw.i_mode;
		mnt->root->uid = mi->raw.i_uid;
		mnt->root->gid = mi->raw.i_gid;
	}
	return 0;
}

int minix_inode_from_raw(struct vfs_mount *mnt, uint32_t ino,
			 const struct minix_raw_inode *raw,
			 struct vfs_inode **out)
{
	struct minix_inode_info *mi;
	struct vfs_inode *inode;

	inode = kzalloc(sizeof(*inode));
	if (!inode)
		return -ENOMEM;

	mi = kzalloc(sizeof(*mi));
	if (!mi) {
		kfree(inode);
		return -ENOMEM;
	}

	memcpy(&mi->raw, raw, sizeof(*raw));
	inode->mnt = mnt;
	inode->ino = ino;
	inode->size = mi->raw.i_size;
	inode->is_dir = (mi->raw.i_mode & 040000) != 0;
	inode->mode = mi->raw.i_mode;
	inode->uid = mi->raw.i_uid;
	inode->gid = mi->raw.i_gid;
	inode->priv = mi;
	mutex_init(&inode->lock);

	*out = inode;
	return 0;
}

void minix_close(struct vfs_inode *ino)
{
	if (ino) {
		if (ino->priv) {
			(void)minix_write_inode(ino->mnt, ino->ino, ino->priv);
			kfree(ino->priv);
		}
		kfree(ino);
	}
}
