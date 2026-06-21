/*
 * kernel/fs/minix/internal.h — Shared MINIX v1 filesystem internals.
 *
 * Defines the on-disk MINIX v1 structures and private helpers used by
 * the split read-only backend. These declarations stay below fs/minix/
 * so the public VFS and MINIX headers do not expose on-disk details.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/fs/block.h>
#include <jnu/base/compiler.h>
#include <jnu/base/types.h>
#include <jnu/fs/vfs.h>
#include <jnu/lib/mutex.h>

#define MINIX_BLOCK_SIZE 1024
#define MINIX_V1_MAGIC 0x137F
#define MINIX_V1_MAGIC_30 0x138F
#define MINIX_NAME_LEN 14
#define MINIX_DIR_ENTRY_SIZE 16

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
	char name[MINIX_NAME_LEN];
} __packed;

struct minix_priv {
	struct mutex bitmap_lock;
	struct minix_super sb;
	uint32_t inodes_start_block;
};

struct minix_inode_info {
	struct minix_raw_inode raw;
	bool dirty;
};

struct minix_buffer {
	struct block_device *bdev;
	uint32_t block_no;
	uint32_t refcount;
	bool dirty;
	uint8_t data[MINIX_BLOCK_SIZE];
};

struct minix_buffer *bufcache_get(struct block_device *bdev, uint32_t block);
void bufcache_mark_dirty(struct minix_buffer *buf);
void bufcache_put(struct minix_buffer *buf);
int bufcache_sync(struct block_device *bdev);
void bufcache_flush_all(void);
void bufcache_log_stats(void);

int minix_get_raw_inode(struct vfs_mount *mnt, uint32_t ino,
			struct minix_raw_inode *out);
uint32_t minix_bmap(struct vfs_mount *mnt, struct minix_inode_info *mi,
		    uint32_t block, bool create);
int minix_write_inode(struct vfs_mount *mnt, uint32_t ino,
		      struct minix_inode_info *mi);
int minix_inode_from_raw(struct vfs_mount *mnt, uint32_t ino,
			 const struct minix_raw_inode *raw,
			 struct vfs_inode **out);
void minix_close(struct vfs_inode *ino);

int minix_lookup(struct vfs_inode *dir, const char *name,
		 struct vfs_inode **out);
int minix_readdir(struct vfs_inode *dir, size_t index, struct vfs_dirent *out);
ssize_t minix_read(struct vfs_inode *ino, uint64_t offset, size_t len,
		   void *buf);
ssize_t minix_write(struct vfs_inode *ino, uint64_t offset, size_t len,
		    const void *buf);
int minix_truncate(struct vfs_inode *ino, uint64_t size);
int minix_create(struct vfs_inode *dir, const char *name, uint16_t mode,
		 struct vfs_inode **out);
int minix_unlink(struct vfs_inode *dir, const char *name);
int minix_mkdir(struct vfs_inode *dir, const char *name, uint16_t mode);
int minix_rmdir(struct vfs_inode *dir, const char *name);
int minix_rename(struct vfs_inode *old_dir, const char *old_name,
		 struct vfs_inode *new_dir, const char *new_name);
int minix_fsync(struct vfs_inode *ino);

uint32_t minix_alloc_inode(struct vfs_mount *mnt);
void minix_free_inode(struct vfs_mount *mnt, uint32_t ino);
uint32_t minix_alloc_zone(struct vfs_mount *mnt);
void minix_free_zone(struct vfs_mount *mnt, uint32_t zone);
