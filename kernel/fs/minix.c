/*
 * kernel/fs/minix.c — MINIX v1 filesystem reader.
 *
 * Implements a read-only MINIX v1 VFS backend. Supports 14-character
 * filenames, direct, single-indirect, and double-indirect blocks.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/block.h>
#include <jnu/compiler.h>
#include <jnu/errno.h>
#include <jnu/klog.h>
#include <jnu/kmalloc.h>
#include <jnu/minix.h>
#include <jnu/string.h>
#include <jnu/vfs.h>

#define MINIX_BLOCK_SIZE 1024
#define MINIX_V1_MAGIC 0x137F
#define MINIX_V1_MAGIC_30 0x138F /* 30-char names not supported here */

struct minix_super {
	uint16_t s_ninodes;
	uint16_t s_nzones;
	uint16_t s_imap_blocks;
	uint16_t s_zmap_blocks;
	uint16_t s_firstdatazone;
	uint16_t s_log_zone_size;
	uint32_t s_max_size;
	uint16_t s_magic;
	uint16_t s_state;
} __packed;

struct minix_raw_inode {
	uint16_t i_mode;
	uint16_t i_uid;
	uint32_t i_size;
	uint32_t i_time;
	uint8_t i_gid;
	uint8_t i_nlinks;
	uint16_t i_zone[9];
} __packed;

struct minix_dir_entry {
	uint16_t inode;
	char name[14];
} __packed;

struct minix_priv {
	struct minix_super sb;
	uint32_t inodes_start_block;
};

struct minix_inode_info {
	struct minix_raw_inode raw;
};

static int minix_read_block(struct vfs_mount *mnt, uint32_t block, void *buf)
{
	uint64_t lba = (uint64_t)block * (MINIX_BLOCK_SIZE / 512);
	return block_read(mnt->bdev, lba, MINIX_BLOCK_SIZE / 512, buf);
}

static int minix_get_raw_inode(struct vfs_mount *mnt, uint32_t ino,
			       struct minix_raw_inode *out)
{
	if (ino == 0)
		return -EINVAL;
	struct minix_priv *priv = mnt->priv;
	if (ino > priv->sb.s_ninodes)
		return -EINVAL;

	uint32_t block =
	    priv->inodes_start_block +
	    (ino - 1) / (MINIX_BLOCK_SIZE / sizeof(struct minix_raw_inode));
	uint32_t offset =
	    ((ino - 1) % (MINIX_BLOCK_SIZE / sizeof(struct minix_raw_inode))) *
	    sizeof(struct minix_raw_inode);

	uint8_t buf[MINIX_BLOCK_SIZE];
	int err = minix_read_block(mnt, block, buf);
	if (err)
		return err;

	memcpy(out, buf + offset, sizeof(*out));
	return 0;
}

static uint32_t minix_bmap(struct vfs_mount *mnt, struct minix_raw_inode *ino,
			   uint32_t block)
{
	struct minix_priv *priv = mnt->priv;
	uint32_t b = 0;

	/* Direct blocks */
	if (block < 7) {
		b = ino->i_zone[block];
		goto check;
	}
	block -= 7;

	/* Single indirect */
	uint16_t per_block = MINIX_BLOCK_SIZE / sizeof(uint16_t);
	if (block < per_block) {
		if (ino->i_zone[7] == 0)
			return 0;
		if (ino->i_zone[7] >= priv->sb.s_nzones) {
			pr_err(
			    "minix: out of bounds single indirect pointer %u\n",
			    ino->i_zone[7]);
			return 0;
		}
		uint16_t buf[MINIX_BLOCK_SIZE / sizeof(uint16_t)];
		if (minix_read_block(mnt, ino->i_zone[7], buf) != 0)
			return 0;
		b = buf[block];
		goto check;
	}
	block -= per_block;

	/* Double indirect */
	if (block < (uint32_t)per_block * per_block) {
		if (ino->i_zone[8] == 0)
			return 0;
		if (ino->i_zone[8] >= priv->sb.s_nzones) {
			pr_err(
			    "minix: out of bounds double indirect pointer %u\n",
			    ino->i_zone[8]);
			return 0;
		}
		uint16_t buf[MINIX_BLOCK_SIZE / sizeof(uint16_t)];
		if (minix_read_block(mnt, ino->i_zone[8], buf) != 0)
			return 0;
		uint32_t ind1 = buf[block / per_block];
		if (ind1 == 0)
			return 0;
		if (ind1 >= priv->sb.s_nzones) {
			pr_err("minix: out of bounds single indirect pointer "
			       "in double indirect %u\n",
			       ind1);
			return 0;
		}
		if (minix_read_block(mnt, ind1, buf) != 0)
			return 0;
		b = buf[block % per_block];
		goto check;
	}

check:
	if (b != 0 && b >= priv->sb.s_nzones) {
		pr_err("minix: out of bounds block pointer %u\n", b);
		return 0;
	}
	return b;
}

static int minix_mount(struct vfs_mount *mnt, struct block_device *bdev)
{
	uint8_t buf[MINIX_BLOCK_SIZE];
	int err = block_read(bdev, 2, 2,
			     buf); /* offset 1024 bytes = LBA 2, 2 sectors */
	if (err)
		return err;

	struct minix_super *sb = (struct minix_super *)buf;
	if (sb->s_magic != MINIX_V1_MAGIC) {
		pr_err("minix: invalid magic 0x%04x\n", sb->s_magic);
		return -EINVAL;
	}

	struct minix_priv *priv = kzalloc(sizeof(*priv));
	if (!priv)
		return -ENOMEM;

	memcpy(&priv->sb, sb, sizeof(*sb));
	priv->inodes_start_block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks;

	mnt->priv = priv;

	/* Create root inode */
	struct vfs_inode *root = kzalloc(sizeof(*root));
	if (!root) {
		kfree(priv);
		return -ENOMEM;
	}

	struct minix_inode_info *mi = kzalloc(sizeof(*mi));
	if (!mi) {
		kfree(root);
		kfree(priv);
		return -ENOMEM;
	}

	err = minix_get_raw_inode(mnt, 1, &mi->raw); /* Root inode is 1 */
	if (err) {
		kfree(mi);
		kfree(root);
		kfree(priv);
		return err;
	}

	root->mnt = mnt;
	root->ino = 1;
	root->size = mi->raw.i_size;
	root->is_dir = (mi->raw.i_mode & 040000) != 0;
	root->mode = mi->raw.i_mode;
	root->uid = mi->raw.i_uid;
	root->gid = mi->raw.i_gid;
	root->priv = mi;

	mnt->root = root;

	return 0;
}

static int minix_lookup(struct vfs_inode *dir, const char *name,
			struct vfs_inode **out)
{
	if (!dir->is_dir)
		return -ENOTDIR;

	struct minix_inode_info *mi = dir->priv;
	uint32_t size = mi->raw.i_size;
	uint32_t offset = 0;
	uint8_t buf[MINIX_BLOCK_SIZE];

	if (strcmp(name, ".") == 0) {
		struct vfs_inode *ino = kzalloc(sizeof(*ino));
		if (!ino)
			return -ENOMEM;
		struct minix_inode_info *new_mi = kzalloc(sizeof(*new_mi));
		if (!new_mi) {
			kfree(ino);
			return -ENOMEM;
		}
		memcpy(new_mi, mi, sizeof(*mi));
		ino->mnt = dir->mnt;
		ino->ino = dir->ino;
		ino->size = dir->size;
		ino->is_dir = dir->is_dir;
		ino->mode = dir->mode;
		ino->uid = dir->uid;
		ino->gid = dir->gid;
		ino->priv = new_mi;
		*out = ino;
		return 0;
	}

	while (offset < size) {
		uint32_t b =
		    minix_bmap(dir->mnt, &mi->raw, offset / MINIX_BLOCK_SIZE);
		if (b == 0) {
			offset += MINIX_BLOCK_SIZE;
			continue;
		}

		if (minix_read_block(dir->mnt, b, buf) != 0)
			return -EIO;

		uint32_t chunk = size - offset;
		if (chunk > MINIX_BLOCK_SIZE)
			chunk = MINIX_BLOCK_SIZE;

		for (uint32_t i = 0; i < chunk;
		     i += sizeof(struct minix_dir_entry)) {
			struct minix_dir_entry *de =
			    (struct minix_dir_entry *)(buf + i);
			if (de->inode == 0)
				continue;

			if (de->inode > ((struct minix_priv *)dir->mnt->priv)
					    ->sb.s_ninodes) {
				pr_err("minix: malicious inode number %u in "
				       "directory\n",
				       de->inode);
				continue;
			}

			char dename[15];
			memcpy(dename, de->name, 14);
			dename[14] = '\0';

			if (strcmp(dename, name) == 0) {
				struct vfs_inode *ino = kzalloc(sizeof(*ino));
				if (!ino)
					return -ENOMEM;

				struct minix_inode_info *new_mi =
				    kzalloc(sizeof(*new_mi));
				if (!new_mi) {
					kfree(ino);
					return -ENOMEM;
				}

				if (minix_get_raw_inode(dir->mnt, de->inode,
							&new_mi->raw) != 0) {
					kfree(new_mi);
					kfree(ino);
					return -EIO;
				}

				ino->mnt = dir->mnt;
				ino->ino = de->inode;
				ino->size = new_mi->raw.i_size;
				ino->is_dir =
				    (new_mi->raw.i_mode & 040000) != 0;
				ino->mode = new_mi->raw.i_mode;
				ino->uid = new_mi->raw.i_uid;
				ino->gid = new_mi->raw.i_gid;
				ino->priv = new_mi;

				*out = ino;
				return 0;
			}
		}
		offset += MINIX_BLOCK_SIZE;
	}

	return -ENOENT;
}

static ssize_t minix_read(struct vfs_inode *ino, uint64_t offset, size_t len,
			  void *buf)
{
	if (offset >= ino->size)
		return 0;
	if (offset + len > ino->size)
		len = (size_t)(ino->size - offset);

	struct minix_inode_info *mi = ino->priv;
	uint8_t *p = buf;
	size_t left = len;
	uint64_t curr_off = offset;

	uint8_t blkbuf[MINIX_BLOCK_SIZE];

	while (left > 0) {
		uint32_t lblk = (uint32_t)(curr_off / MINIX_BLOCK_SIZE);
		uint32_t boff = (uint32_t)(curr_off % MINIX_BLOCK_SIZE);
		uint32_t pblk = minix_bmap(ino->mnt, &mi->raw, lblk);

		uint32_t chunk = MINIX_BLOCK_SIZE - boff;
		if (chunk > left)
			chunk = (uint32_t)left;

		if (pblk == 0) {
			memset(p, 0, chunk);
		} else {
			if (minix_read_block(ino->mnt, pblk, blkbuf) != 0)
				return -EIO;
			memcpy(p, blkbuf + boff, chunk);
		}

		p += chunk;
		left -= chunk;
		curr_off += chunk;
	}

	return (ssize_t)len;
}

static void minix_close(struct vfs_inode *ino)
{
	if (ino) {
		if (ino->priv)
			kfree(ino->priv);
		kfree(ino);
	}
}

static int minix_readdir(struct vfs_inode *dir, size_t index,
			 struct vfs_dirent *out)
{
	if (!dir->is_dir)
		return -ENOTDIR;

	struct minix_inode_info *mi = dir->priv;
	uint32_t size = mi->raw.i_size;
	uint32_t offset = 0;
	uint8_t buf[MINIX_BLOCK_SIZE];
	size_t curr_idx = 0;

	while (offset < size) {
		uint32_t b =
		    minix_bmap(dir->mnt, &mi->raw, offset / MINIX_BLOCK_SIZE);
		if (b != 0) {
			if (minix_read_block(dir->mnt, b, buf) != 0)
				return -EIO;

			uint32_t chunk = size - offset;
			if (chunk > MINIX_BLOCK_SIZE)
				chunk = MINIX_BLOCK_SIZE;

			for (uint32_t i = 0; i < chunk;
			     i += sizeof(struct minix_dir_entry)) {
				struct minix_dir_entry *de =
				    (struct minix_dir_entry *)(buf + i);
				if (de->inode == 0)
					continue;

				if (de->inode >
				    ((struct minix_priv *)dir->mnt->priv)
					->sb.s_ninodes) {
					pr_err("minix: malicious inode number "
					       "%u in directory\n",
					       de->inode);
					continue;
				}

				if (curr_idx == index) {
					out->ino = de->inode;
					memcpy(out->name, de->name, 14);
					out->name[14] = '\0';
					return 1; /* Found */
				}
				curr_idx++;
			}
		}
		offset += MINIX_BLOCK_SIZE;
	}

	return 0; /* EOF */
}

const struct vfs_ops minix_ops = {
    .mount = minix_mount,
    .lookup = minix_lookup,
    .readdir = minix_readdir,
    .read = minix_read,
    .close = minix_close,
};

int minix_selftest(void)
{
	struct vfs_inode *ino;
	int err = vfs_open("/", &ino);
	if (err) {
		pr_err("minix_selftest: could not open /\n");
		return err;
	}

	if (!ino->is_dir) {
		pr_err("minix_selftest: / is not a directory\n");
		vfs_close(ino);
		return -ENOTDIR;
	}

	/* Count entries */
	struct vfs_dirent de;
	size_t idx = 0;
	int count = 0;
	while (minix_readdir(ino, idx++, &de) == 1) {
		count++;
	}

	vfs_close(ino);

	if (count < 2) {
		pr_err("minix_selftest: / has too few entries (%d)\n", count);
		return -EIO;
	}

	pr_info("minix_selftest: [ OK ]\n");
	return 0;
}
