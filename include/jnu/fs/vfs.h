/*
 * include/jnu/fs/vfs.h — Read-only Virtual File System API.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/base/types.h>
#include <jnu/lib/mutex.h>

struct vfs_inode;
struct vfs_mount;
struct block_device;

#define VFS_NAME_MAX 64

struct vfs_dirent {
	uint32_t ino;
	char name[VFS_NAME_MAX];
};

struct vfs_ops {
	int (*mount)(struct vfs_mount *mnt, struct block_device *bdev);
	int (*lookup)(struct vfs_inode *dir, const char *name,
		      struct vfs_inode **out);
	int (*readdir)(struct vfs_inode *dir, size_t index,
		       struct vfs_dirent *out);
	ssize_t (*read)(struct vfs_inode *ino, uint64_t offset, size_t len,
			void *buf);
	ssize_t (*write)(struct vfs_inode *ino, uint64_t offset, size_t len,
			 const void *buf);
	int (*truncate)(struct vfs_inode *ino, uint64_t size);
	int (*create)(struct vfs_inode *dir, const char *name, uint16_t mode,
		      struct vfs_inode **out);
	int (*unlink)(struct vfs_inode *dir, const char *name);
	int (*mkdir)(struct vfs_inode *dir, const char *name, uint16_t mode);
	int (*rmdir)(struct vfs_inode *dir, const char *name);
	int (*rename)(struct vfs_inode *old_dir, const char *old_name,
		      struct vfs_inode *new_dir, const char *new_name);
	int (*fsync)(struct vfs_inode *ino);
	void (*close)(struct vfs_inode *ino);
};

struct vfs_mount {
	struct block_device *bdev;
	const struct vfs_ops *ops;
	void *priv;
	struct vfs_inode *root;
};

struct vfs_inode {
	struct mutex lock;
	struct vfs_mount *mnt;
	uint32_t ino;
	uint64_t size;
	bool is_dir;
	uint16_t mode;
	uint16_t uid;
	uint16_t gid;
	void *priv;

	/*
	 * Inode-cache bookkeeping, owned by vfs.c. A cached inode is the
	 * single canonical in-memory object for its (mnt, ino) pair;
	 * `refcount` counts live references and `cache_next` chains hash
	 * collisions. A freshly built inode that has not (yet) been
	 * canonicalized has refcount == 0 and is not on any chain.
	 */
	uint32_t refcount;
	struct vfs_inode *cache_next;
};

void vfs_init(void);

int vfs_mount(const char *bdev_name, const char *fstype, const char *target);

int vfs_open(const char *path, struct vfs_inode **out);

ssize_t vfs_read(struct vfs_inode *ino, uint64_t offset, size_t len, void *buf);
ssize_t vfs_write(struct vfs_inode *ino, uint64_t offset, size_t len,
		  const void *buf);
int vfs_truncate(struct vfs_inode *ino, uint64_t size);
int vfs_create(const char *path, uint16_t mode, struct vfs_inode **out);
int vfs_unlink(const char *path);
int vfs_mkdir(const char *path, uint16_t mode);
int vfs_rmdir(const char *path);
int vfs_rename(const char *old_path, const char *new_path);
int vfs_fsync(struct vfs_inode *ino);

int vfs_readdir(struct vfs_inode *dir, size_t index, struct vfs_dirent *out);

void vfs_close(struct vfs_inode *ino);

int vfs_selftest(void);
