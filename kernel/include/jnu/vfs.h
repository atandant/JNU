/*
 * include/jnu/vfs.h — Read-only Virtual File System API.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>

struct vfs_inode;
struct vfs_mount;
struct block_device;

#define VFS_NAME_MAX 64

struct vfs_dirent {
	uint32_t	ino;
	char		name[VFS_NAME_MAX];
};

struct vfs_ops {
	int (*mount)(struct vfs_mount *mnt, struct block_device *bdev);
	int (*lookup)(struct vfs_inode *dir, const char *name, struct vfs_inode **out);
	int (*readdir)(struct vfs_inode *dir, size_t index, struct vfs_dirent *out);
	ssize_t (*read)(struct vfs_inode *ino, uint64_t offset, size_t len, void *buf);
	void (*close)(struct vfs_inode *ino);
};

struct vfs_mount {
	struct block_device	*bdev;
	const struct vfs_ops	*ops;
	void			*priv;
	struct vfs_inode	*root;
};

struct vfs_inode {
	struct vfs_mount	*mnt;
	uint32_t		ino;
	uint64_t		size;
	bool			is_dir;
	uint16_t		mode;
	uint16_t		uid;
	uint16_t		gid;
	void			*priv;
};

void vfs_init(void);

int vfs_mount(const char *bdev_name, const char *fstype, const char *target);

int vfs_open(const char *path, struct vfs_inode **out);

ssize_t vfs_read(struct vfs_inode *ino, uint64_t offset, size_t len, void *buf);

int vfs_readdir(struct vfs_inode *dir, size_t index, struct vfs_dirent *out);

void vfs_close(struct vfs_inode *ino);

int vfs_selftest(void);
