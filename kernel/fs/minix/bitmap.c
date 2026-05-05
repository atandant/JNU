/*
 * kernel/fs/minix/bitmap.c — MINIX v1 inode and zone bitmap allocators.
 *
 * Walks the on-disk imap and zmap through the buffer cache. Allocation
 * sets the first clear bit and marks that bitmap block dirty; free clears
 * the bit and panics on double-free so metadata corruption is caught at
 * the mutator boundary.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "internal.h"

#include <jnu/errno.h>
#include <jnu/klog.h>
#include <jnu/minix.h>
#include <jnu/panic.h>
#include <jnu/string.h>

#define MINIX_BITS_PER_BLOCK (MINIX_BLOCK_SIZE * 8)
#define BITMAP_SELFTEST_BLOCKS 8

static bool minix_test_bit(const uint8_t *map, uint32_t bit)
{
	return (map[bit / 8] & (uint8_t)(1u << (bit % 8))) != 0;
}

static void minix_set_bit(uint8_t *map, uint32_t bit)
{
	map[bit / 8] = (uint8_t)(map[bit / 8] | (uint8_t)(1u << (bit % 8)));
}

static void minix_clear_bit(uint8_t *map, uint32_t bit)
{
	map[bit / 8] = (uint8_t)(map[bit / 8] & (uint8_t)~(1u << (bit % 8)));
}

static uint32_t minix_alloc_from_map(struct vfs_mount *mnt, uint32_t start_block,
				     uint32_t blocks, uint32_t limit)
{
	for (uint32_t blk = 0; blk < blocks; blk++) {
		struct minix_buffer *buf;
		uint32_t base = blk * MINIX_BITS_PER_BLOCK;

		buf = bufcache_get(mnt->bdev, start_block + blk);
		if (!buf)
			return 0;

		for (uint32_t bit = 0; bit < MINIX_BITS_PER_BLOCK; bit++) {
			uint32_t idx = base + bit;

			if (idx == 0)
				continue;
			if (idx > limit)
				break;
			if (minix_test_bit(buf->data, bit))
				continue;

			minix_set_bit(buf->data, bit);
			bufcache_mark_dirty(buf);
			bufcache_put(buf);
			return idx;
		}

		bufcache_put(buf);
	}

	return 0;
}

static void minix_free_from_map(struct vfs_mount *mnt, uint32_t start_block,
				uint32_t idx)
{
	struct minix_buffer *buf;
	uint32_t blk = idx / MINIX_BITS_PER_BLOCK;
	uint32_t bit = idx % MINIX_BITS_PER_BLOCK;

	buf = bufcache_get(mnt->bdev, start_block + blk);
	if (!buf)
		panic("minix: failed to read bitmap block for free");

	if (!minix_test_bit(buf->data, bit))
		panic("minix: double-free of bitmap bit %u", idx);

	minix_clear_bit(buf->data, bit);
	bufcache_mark_dirty(buf);
	bufcache_put(buf);
}

uint32_t minix_alloc_inode(struct vfs_mount *mnt)
{
	struct minix_priv *priv = mnt->priv;

	return minix_alloc_from_map(mnt, 2, priv->sb.s_imap_blocks,
				    priv->sb.s_ninodes);
}

void minix_free_inode(struct vfs_mount *mnt, uint32_t ino)
{
	struct minix_priv *priv = mnt->priv;

	if (ino == 0 || ino > priv->sb.s_ninodes)
		panic("minix: free invalid inode %u", ino);

	minix_free_from_map(mnt, 2, ino);
}

uint32_t minix_alloc_zone(struct vfs_mount *mnt)
{
	struct minix_priv *priv = mnt->priv;
	uint32_t zidx;

	zidx = minix_alloc_from_map(mnt, 2 + priv->sb.s_imap_blocks,
				    priv->sb.s_zmap_blocks,
				    priv->sb.s_nzones - priv->sb.s_firstdatazone);
	if (zidx == 0)
		return 0;

	return priv->sb.s_firstdatazone + zidx - 1;
}

void minix_free_zone(struct vfs_mount *mnt, uint32_t zone)
{
	struct minix_priv *priv = mnt->priv;
	uint32_t zidx;

	if (zone < priv->sb.s_firstdatazone || zone >= priv->sb.s_nzones)
		panic("minix: free invalid zone %u", zone);

	zidx = zone - priv->sb.s_firstdatazone + 1;
	minix_free_from_map(mnt, 2 + priv->sb.s_imap_blocks, zidx);
}

static uint8_t bitmap_selftest_storage[BITMAP_SELFTEST_BLOCKS][MINIX_BLOCK_SIZE];

static int bitmap_selftest_read(struct block_device *bdev, uint64_t lba,
				size_t count, void *buf)
{
	(void)bdev;
	if (count != MINIX_BLOCK_SIZE / 512)
		return -EINVAL;
	if ((lba % (MINIX_BLOCK_SIZE / 512)) != 0)
		return -EINVAL;
	if (lba / (MINIX_BLOCK_SIZE / 512) >= BITMAP_SELFTEST_BLOCKS)
		return -EINVAL;
	memcpy(buf, bitmap_selftest_storage[lba / (MINIX_BLOCK_SIZE / 512)],
	       MINIX_BLOCK_SIZE);
	return 0;
}

static int bitmap_selftest_write(struct block_device *bdev, uint64_t lba,
				 size_t count, const void *buf)
{
	(void)bdev;
	if (count != MINIX_BLOCK_SIZE / 512)
		return -EINVAL;
	if ((lba % (MINIX_BLOCK_SIZE / 512)) != 0)
		return -EINVAL;
	if (lba / (MINIX_BLOCK_SIZE / 512) >= BITMAP_SELFTEST_BLOCKS)
		return -EINVAL;
	memcpy(bitmap_selftest_storage[lba / (MINIX_BLOCK_SIZE / 512)], buf,
	       MINIX_BLOCK_SIZE);
	return 0;
}

static const struct block_ops bitmap_selftest_ops = {
    .read = bitmap_selftest_read,
    .write = bitmap_selftest_write,
};

int minix_bitmap_selftest(void)
{
	struct minix_priv priv;
	struct vfs_mount mnt;
	struct block_device bdev = {
	    .name = "bitmap-test",
	    .sector_size = 512,
	    .sector_count = BITMAP_SELFTEST_BLOCKS * (MINIX_BLOCK_SIZE / 512),
	    .ops = &bitmap_selftest_ops,
	};
	uint32_t ino;
	uint32_t zone;
	int err;

	memset(bitmap_selftest_storage, 0, sizeof(bitmap_selftest_storage));
	memset(&priv, 0, sizeof(priv));
	memset(&mnt, 0, sizeof(mnt));

	priv.sb.s_ninodes = 16;
	priv.sb.s_nzones = 32;
	priv.sb.s_imap_blocks = 1;
	priv.sb.s_zmap_blocks = 1;
	priv.sb.s_firstdatazone = 5;
	mnt.bdev = &bdev;
	mnt.priv = &priv;

	ino = minix_alloc_inode(&mnt);
	if (ino != 1)
		return -EIO;
	zone = minix_alloc_zone(&mnt);
	if (zone != priv.sb.s_firstdatazone)
		return -EIO;
	err = bufcache_sync(&bdev);
	if (err)
		return err;
	if ((bitmap_selftest_storage[2][0] & 0x02u) == 0 ||
	    (bitmap_selftest_storage[3][0] & 0x02u) == 0)
		return -EIO;

	minix_free_inode(&mnt, ino);
	minix_free_zone(&mnt, zone);
	err = bufcache_sync(&bdev);
	if (err)
		return err;
	if ((bitmap_selftest_storage[2][0] & 0x02u) != 0 ||
	    (bitmap_selftest_storage[3][0] & 0x02u) != 0)
		return -EIO;

	pr_info("minix_bitmap_selftest: [ OK ]\n");
	return 0;
}
