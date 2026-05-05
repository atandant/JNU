/*
 * kernel/fs/minix/super.c — MINIX v1 mount and superblock handling.
 *
 * Owns the VFS operations table and mount-time superblock parsing for
 * the read-only MINIX backend. Root inode creation is delegated to the
 * shared inode helpers after the superblock is cached.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "internal.h"

#include <jnu/errno.h>
#include <jnu/klog.h>
#include <jnu/kmalloc.h>
#include <jnu/minix.h>
#include <jnu/string.h>

static int minix_validate_super(const struct minix_super *sb,
				const struct block_device *bdev)
{
	uint32_t inode_blocks;
	uint32_t metadata_blocks;
	uint32_t disk_blocks;

	if (sb->s_magic != MINIX_V1_MAGIC)
		return -EINVAL;
	if (sb->s_ninodes == 0 || sb->s_nzones == 0)
		return -EINVAL;
	if (sb->s_imap_blocks == 0 || sb->s_zmap_blocks == 0)
		return -EINVAL;
	if (sb->s_log_zone_size != 0)
		return -EINVAL;
	if (sb->s_firstdatazone < 2)
		return -EINVAL;
	if (sb->s_firstdatazone >= sb->s_nzones)
		return -EINVAL;
	if (bdev->sector_size != 512)
		return -EINVAL;

	inode_blocks =
		((uint32_t)sb->s_ninodes * sizeof(struct minix_raw_inode) +
		 MINIX_BLOCK_SIZE - 1u) /
		MINIX_BLOCK_SIZE;
	metadata_blocks = 2u + sb->s_imap_blocks + sb->s_zmap_blocks +
			  inode_blocks;
	if (metadata_blocks > sb->s_firstdatazone)
		return -EINVAL;

	disk_blocks = (uint32_t)(bdev->sector_count / (MINIX_BLOCK_SIZE / 512));
	if (sb->s_nzones > disk_blocks)
		return -EINVAL;

	return 0;
}

/*
 * Mount a MINIX v1 filesystem from a block device.
 *
 * Returns:
 *   0 on success, negative errno on failure.
 */
static int minix_mount(struct vfs_mount *mnt, struct block_device *bdev)
{
	struct minix_raw_inode root_raw;
	struct minix_buffer *buf;
	struct minix_super *sb;
	struct minix_priv *priv;
	struct vfs_inode *root;
	int err;

	buf = bufcache_get(bdev, 1);
	if (!buf)
		return -EIO;

	sb = (struct minix_super *)buf->data;
	err = minix_validate_super(sb, bdev);
	if (err) {
		pr_err("minix: invalid magic 0x%04x\n", sb->s_magic);
		bufcache_put(buf);
		return err;
	}

	priv = kzalloc(sizeof(*priv));
	if (!priv) {
		bufcache_put(buf);
		return -ENOMEM;
	}

	memcpy(&priv->sb, sb, sizeof(*sb));
	priv->inodes_start_block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks;
	bufcache_put(buf);

	mnt->priv = priv;

	err = minix_get_raw_inode(mnt, 1, &root_raw);
	if (err)
		goto fail_priv;

	err = minix_inode_from_raw(mnt, 1, &root_raw, &root);
	if (err)
		goto fail_priv;

	mnt->root = root;
	return 0;

fail_priv:
	mnt->priv = NULL;
	kfree(priv);
	return err;
}

const struct vfs_ops minix_ops = {
    .mount = minix_mount,
    .lookup = minix_lookup,
    .readdir = minix_readdir,
    .read = minix_read,
    .write = minix_write,
    .truncate = minix_truncate,
    .create = minix_create,
    .unlink = minix_unlink,
    .mkdir = minix_mkdir,
    .rmdir = minix_rmdir,
    .rename = minix_rename,
    .fsync = minix_fsync,
    .close = minix_close,
};

int minix_fsync(struct vfs_inode *ino)
{
	struct minix_inode_info *mi = ino->priv;
	int err;

	err = minix_write_inode(ino->mnt, ino->ino, mi);
	if (err)
		return err;
	return bufcache_sync(ino->mnt->bdev);
}

int minix_selftest(void)
{
	struct vfs_inode *ino;
	struct vfs_dirent de;
	size_t idx = 0;
	int count = 0;
	int err;

	err = vfs_open("/", &ino);
	if (err) {
		pr_err("minix_selftest: could not open /\n");
		return err;
	}

	if (!ino->is_dir) {
		pr_err("minix_selftest: / is not a directory\n");
		vfs_close(ino);
		return -ENOTDIR;
	}

	while (minix_readdir(ino, idx++, &de) == 1)
		count++;

	vfs_close(ino);

	if (count < 2) {
		pr_err("minix_selftest: / has too few entries (%d)\n", count);
		return -EIO;
	}

	pr_info("minix_selftest: [ OK ]\n");
	return 0;
}

int minix_fsync_selftest(void)
{
	struct vfs_inode *ino;
	int err;

	err = vfs_create("/c2-fsync", 0666, &ino);
	if (err == -EEXIST)
		err = vfs_open("/c2-fsync", &ino);
	if (err)
		return err;
	err = vfs_fsync(ino);
	vfs_close(ino);
	(void)vfs_unlink("/c2-fsync");
	if (err)
		return err;
	pr_info("minix_fsync_selftest: [ OK ]\n");
	return 0;
}
