/*
 * kernel/fs/fat32/buffer.c — FAT32-local 512 B sector cache.
 *
 * A fixed 64-slot LRU cache keyed by (block_device, lba). This is the
 * "Path 2" private cache: FAT32 ships its own read-only sector cache so
 * the just-shipped MINIX 1 KiB buffer cache is left untouched. Once a
 * second consumer (FAT32) exists, the right generic, variable-block-size
 * cache shape becomes obvious and the two can be merged ("Path 1").
 *
 * The cache is read-only: there is no dirty list or writeback path
 * because the v0.0.4 FAT32 backend never writes.
 *
 * Lock ordering: fat32_cache_lock is a leaf (same level as the MINIX
 * bufcache_lock): bufcache_lock < pmm_lock < vfs_inode_lock.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "internal.h"

#include <jnu/kernel/sched.h>
#include <jnu/lib/klog.h>
#include <jnu/lib/mutex.h>
#include <jnu/lib/string.h>
#include <uapi/jnu/errno.h>

#define FAT32_CACHE_SLOTS 64

struct fat32_slot {
	struct fat32_buffer buf;
	bool valid;
	bool loading; /* in-flight block_read; (bdev,lba) published */
	uint64_t lru;
};

static struct fat32_slot slots[FAT32_CACHE_SLOTS];
static struct mutex fat32_cache_lock = MUTEX_INITIALIZER;
static uint64_t fat32_cache_clock;

/*
 * Pick a slot for a cache miss. Invalid slots are preferred; otherwise
 * the oldest unreferenced slot is reused. Returns NULL only if every
 * slot is currently referenced (cache pressure).
 */
static struct fat32_slot *fat32_pick_victim(void)
{
	struct fat32_slot *victim = NULL;

	for (size_t i = 0; i < FAT32_CACHE_SLOTS; i++) {
		/* Never evict a slot whose load is still in flight. */
		if (slots[i].loading)
			continue;
		if (!slots[i].valid && slots[i].buf.refcount == 0)
			return &slots[i];
		if (slots[i].buf.refcount != 0)
			continue;
		if (!victim || slots[i].lru < victim->lru)
			victim = &slots[i];
	}

	return victim;
}

/*
 * Return a referenced cache buffer for one 512 B sector.
 *
 * Returns a referenced buffer on success, or NULL on I/O failure or
 * cache pressure. Drop the reference with fat32_bput().
 */
struct fat32_buffer *fat32_bget(struct block_device *bdev, uint64_t lba)
{
	struct fat32_slot *slot;
	int err;

	if (!bdev || bdev->sector_size != FAT32_SECTOR_SIZE)
		return NULL;

retry:
	mutex_lock(&fat32_cache_lock);
	for (size_t i = 0; i < FAT32_CACHE_SLOTS; i++) {
		if (slots[i].buf.bdev != bdev || slots[i].buf.lba != lba)
			continue;
		if (!slots[i].valid && !slots[i].loading)
			continue;
		if (slots[i].loading) {
			/*
			 * Another thread is loading this exact (bdev, lba)
			 * right now. Wait for it instead of picking a fresh
			 * victim and creating a duplicate slot for one sector.
			 * Single-CPU + IRQ-disable makes this unreachable
			 * today; the structure is in place for SMP, matching
			 * the MINIX bufcache pattern.
			 */
			mutex_unlock(&fat32_cache_lock);
			sched_yield();
			goto retry;
		}
		slots[i].buf.refcount++;
		slots[i].lru = ++fat32_cache_clock;
		mutex_unlock(&fat32_cache_lock);
		return &slots[i].buf;
	}

	slot = fat32_pick_victim();
	if (!slot) {
		mutex_unlock(&fat32_cache_lock);
		pr_err("fat32: no free sector-cache slot for lba %llu\n",
		       (unsigned long long)lba);
		return NULL;
	}

	slot->valid = false;
	slot->loading = true; /* publish (bdev, lba) before dropping lock */
	slot->buf.bdev = bdev;
	slot->buf.lba = lba;
	slot->buf.refcount = 1;
	slot->lru = ++fat32_cache_clock;
	mutex_unlock(&fat32_cache_lock);

	err = block_read(bdev, lba, 1, slot->buf.data);
	if (err) {
		mutex_lock(&fat32_cache_lock);
		slot->valid = false;
		slot->loading = false;
		slot->buf.refcount = 0;
		mutex_unlock(&fat32_cache_lock);
		return NULL;
	}

	mutex_lock(&fat32_cache_lock);
	slot->valid = true;
	slot->loading = false;
	mutex_unlock(&fat32_cache_lock);
	return &slot->buf;
}

/*
 * Drop a cache buffer reference.
 */
void fat32_bput(struct fat32_buffer *buf)
{
	if (!buf)
		return;

	mutex_lock(&fat32_cache_lock);
	if (buf->refcount == 0) {
		mutex_unlock(&fat32_cache_lock);
		pr_err("fat32: bput on unreferenced lba %llu\n",
		       (unsigned long long)buf->lba);
		return;
	}
	buf->refcount--;
	mutex_unlock(&fat32_cache_lock);
}
