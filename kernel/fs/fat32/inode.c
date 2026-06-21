/*
 * kernel/fs/fat32/inode.c — vfs_inode construction and teardown.
 *
 * FAT32 has no inode numbers, so identity is synthesized from the
 * on-disk location of a file's directory entry (see fat32_dirent_ino in
 * dir.c). The root directory has no directory entry of its own and uses
 * the reserved sentinel FAT32_ROOT_INO.
 *
 * FAT has no per-file owner or permission bits, so directories are
 * presented as mode 0555 and regular files as 0444 (the volume is
 * read-only). A FAT32 volume can be marked read-only as a whole, but
 * v0.0.4 never writes regardless.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "internal.h"

#include <jnu/lib/mutex.h>
#include <jnu/lib/string.h>
#include <jnu/mm/kmalloc.h>
#include <uapi/jnu/errno.h>

#define FAT32_MODE_DIR 0555
#define FAT32_MODE_REG 0444

static int fat32_inode_alloc(struct vfs_mount *mnt, uint32_t ino,
			     uint32_t first_cluster, uint8_t attr, bool is_dir,
			     uint64_t size, struct vfs_inode **out)
{
	struct fat32_inode_info *fi;
	struct vfs_inode *inode;

	inode = kzalloc(sizeof(*inode));
	if (!inode)
		return -ENOMEM;

	fi = kzalloc(sizeof(*fi));
	if (!fi) {
		kfree(inode);
		return -ENOMEM;
	}

	fi->first_cluster = first_cluster;
	fi->attr = attr;

	inode->mnt = mnt;
	inode->ino = ino;
	inode->is_dir = is_dir;
	inode->size = is_dir ? 0 : size;
	inode->mode = is_dir ? FAT32_MODE_DIR : FAT32_MODE_REG;
	inode->uid = 0;
	inode->gid = 0;
	inode->priv = fi;
	mutex_init(&inode->lock);

	*out = inode;
	return 0;
}

/*
 * Build a vfs_inode from a directory entry. The starting cluster is
 * reassembled from its high and low 16-bit halves.
 */
int fat32_inode_build(struct vfs_mount *mnt, uint32_t ino,
		      const struct fat32_dirent *de, struct vfs_inode **out)
{
	uint32_t first = ((uint32_t)de->fst_clus_hi << 16) | de->fst_clus_lo;
	bool is_dir = (de->attr & FAT32_ATTR_DIRECTORY) != 0;

	return fat32_inode_alloc(mnt, ino, first, de->attr, is_dir,
				 de->file_size, out);
}

/*
 * Build the root-directory inode. The root has no on-disk directory
 * entry, so its starting cluster comes from the BPB and its identity is
 * the reserved sentinel.
 */
int fat32_inode_root(struct vfs_mount *mnt, struct vfs_inode **out)
{
	struct fat32_priv *priv = mnt->priv;

	return fat32_inode_alloc(mnt, FAT32_ROOT_INO, priv->root_clus,
				 FAT32_ATTR_DIRECTORY, true, 0, out);
}

/*
 * Build an independent copy of an existing inode (used for lookup(".")).
 * The VFS inode cache folds the copy back onto the canonical object.
 */
int fat32_clone_inode(struct vfs_inode *src, struct vfs_inode **out)
{
	struct fat32_inode_info *sfi = src->priv;

	return fat32_inode_alloc(src->mnt, src->ino, sfi->first_cluster,
				 sfi->attr, src->is_dir, src->size, out);
}

/*
 * Release an inode. The FAT32 backend is read-only, so there is no
 * writeback — just free the private info and the inode itself. This is
 * the single teardown callback the VFS inode cache invokes on the last
 * reference.
 */
void fat32_close(struct vfs_inode *ino)
{
	if (ino) {
		kfree(ino->priv);
		kfree(ino);
	}
}
