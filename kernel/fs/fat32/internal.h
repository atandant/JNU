/*
 * kernel/fs/fat32/internal.h — Shared FAT32 filesystem internals.
 *
 * Defines the on-disk FAT32 structures and the private helpers used by
 * the read-only backend. These declarations stay below fs/fat32/ so the
 * public VFS and FAT32 headers do not expose on-disk details.
 *
 * v0.0.4 ships FAT32 read-only with 8.3 short names. Long File Names
 * (LFN) are intentionally deferred — see the TODO in dir.c and the
 * VFS_NAME_MAX note in include/jnu/fs/vfs.h.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/base/compiler.h>
#include <jnu/base/types.h>
#include <jnu/fs/block.h>
#include <jnu/fs/vfs.h>
#include <jnu/lib/mutex.h>

#define FAT32_SECTOR_SIZE 512
#define FAT32_DIRENT_SIZE 32
#define FAT32_NAME_LEN 11 /* 8.3 name as packed on disk (8 + 3) */
#define FAT32_ROOT_INO 1  /* sentinel: the root dir has no dir entry */

/* Directory-entry attribute bits (offset 11). */
#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN 0x02
#define FAT32_ATTR_SYSTEM 0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE 0x20
/* A long-name slot sets RO|HID|SYS|VOL simultaneously. */
#define FAT32_ATTR_LONG_NAME 0x0f

/* Directory-entry name[0] sentinels. */
#define FAT32_DIRENT_END 0x00  /* no further entries in this directory */
#define FAT32_DIRENT_FREE 0xe5 /* this slot is free (deleted) */
#define FAT32_DIRENT_E5 0x05   /* real 0xe5 first byte, escaped to 0x05 */

/* FAT entries are 28-bit; the top nibble is reserved. */
#define FAT32_ENTRY_MASK 0x0fffffffu
#define FAT32_EOC_MIN 0x0ffffff8u  /* >= this marks end of cluster chain */
#define FAT32_BAD_CLUSTER 0x0ffffff7u
#define FAT32_MIN_DATA_CLUSTERS 65525u /* below this it is FAT12/FAT16 */

struct fat32_dirent {
	uint8_t name[FAT32_NAME_LEN];
	uint8_t attr;
	uint8_t nt_reserved;
	uint8_t crt_time_tenth;
	uint16_t crt_time;
	uint16_t crt_date;
	uint16_t last_acc_date;
	uint16_t fst_clus_hi;
	uint16_t wrt_time;
	uint16_t wrt_date;
	uint16_t fst_clus_lo;
	uint32_t file_size;
} __packed;

struct fat32_priv {
	uint32_t bytes_per_sec; /* == FAT32_SECTOR_SIZE */
	uint32_t sec_per_clus;  /* power of two, 1..128 */
	uint32_t rsvd_sec;      /* reserved sectors before the first FAT */
	uint32_t num_fats;      /* number of FAT copies */
	uint32_t fat_sz;        /* sectors per FAT */
	uint32_t root_clus;     /* first cluster of the root directory */
	uint32_t data_start_sec; /* first sector of the data region */
	uint32_t total_sectors;
	uint32_t cluster_count;  /* number of data clusters */
	uint32_t cluster_bytes;  /* sec_per_clus * bytes_per_sec */
};

struct fat32_inode_info {
	uint32_t first_cluster;
	uint8_t attr;
};

struct fat32_buffer {
	struct block_device *bdev;
	uint64_t lba;
	uint32_t refcount;
	uint8_t data[FAT32_SECTOR_SIZE];
};

/* Little-endian field readers (FAT is always little-endian). */
static inline uint16_t fat32_rd16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t fat32_rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline bool fat32_is_eoc(uint32_t cluster)
{
	return cluster >= FAT32_EOC_MIN;
}

/* buffer.c — read-only 512 B sector cache (FAT32's own; Path 2). */
struct fat32_buffer *fat32_bget(struct block_device *bdev, uint64_t lba);
void fat32_bput(struct fat32_buffer *buf);

/* fat.c — cluster-chain arithmetic and the FAT walk. */
uint64_t fat32_cluster_lba(const struct fat32_priv *priv, uint32_t cluster);
bool fat32_cluster_valid(const struct fat32_priv *priv, uint32_t cluster);
uint32_t fat32_next_cluster(struct vfs_mount *mnt, uint32_t cluster);
/* Pure helper, exposed for the parser selftest. */
uint32_t fat32_fat_entry(const uint8_t *fat_sector, uint32_t off_in_sector);

/* inode.c — vfs_inode construction and teardown. */
int fat32_inode_build(struct vfs_mount *mnt, uint32_t ino,
		      const struct fat32_dirent *de, struct vfs_inode **out);
int fat32_inode_root(struct vfs_mount *mnt, struct vfs_inode **out);
int fat32_clone_inode(struct vfs_inode *src, struct vfs_inode **out);
void fat32_close(struct vfs_inode *ino);

/* dir.c — directory lookup and enumeration. */
int fat32_lookup(struct vfs_inode *dir, const char *name,
		 struct vfs_inode **out);
int fat32_readdir(struct vfs_inode *dir, size_t index, struct vfs_dirent *out);
/* Pure helper, exposed for the parser selftest. */
void fat32_decode_83(const uint8_t name[FAT32_NAME_LEN], char *out);

/* file.c — regular-file reads. */
ssize_t fat32_read(struct vfs_inode *ino, uint64_t offset, size_t len,
		   void *buf);
