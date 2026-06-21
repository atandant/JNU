/*
 * kernel/fs/minix/buffer.c — MINIX-local 1 KiB buffer cache.
 *
 * Implements a fixed 64-slot LRU cache keyed by (block_device, block).
 * Chunk 1 is read-only: dirty buffers are represented so eviction rules
 * are already correct, but writeback is intentionally a no-op until the
 * write-support chunk wires block writes into the block layer.
 *
 * Lock ordering: bufcache_lock < pmm_lock < vfs_inode_lock.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "internal.h"

#include <jnu/fs/minix.h>
#include <jnu/kernel/sched.h>
#include <jnu/lib/klog.h>
#include <jnu/lib/mutex.h>
#include <jnu/lib/string.h>
#include <uapi/jnu/errno.h>

#define BUFCACHE_SLOTS 64
#define BUFCACHE_DIRTY_LIMIT 48
#define BUFCACHE_SECTORS_PER_BLOCK (MINIX_BLOCK_SIZE / 512)
#define BUFCACHE_SELFTEST_BLOCKS 96

/*
 * v0.0.3.1 audit item 3: a `loading` flag closes the get/read race.
 * When a miss starts an in-flight block_read for (bdev, block), the
 * slot is marked loading=true, valid=false, and `key` (bdev/block_no)
 * is set so a concurrent bufcache_get for the same key can detect the
 * pending load and wait instead of picking another victim and
 * creating a duplicate slot.  Single-CPU + IRQ-disable hides the race
 * today; the wait loop is therefore a placeholder that compiles to a
 * relax+retry under the existing spinlock semantics.
 */
struct bufcache_slot {
	struct minix_buffer buf;
	bool valid;
	bool loading;
	uint64_t lru;
};

static struct bufcache_slot slots[BUFCACHE_SLOTS];
static struct mutex bufcache_mutex = MUTEX_INITIALIZER;
static uint64_t bufcache_clock;
static uint32_t bufcache_dirty_count;
static uint64_t bufcache_hits;
static uint64_t bufcache_misses;

/*
 * Pick a slot for a cache miss.
 *
 * Invalid slots are preferred. Otherwise the oldest clean unreferenced
 * slot is selected; dirty slots become evictable only after chunk 2
 * provides writeback.
 */
static struct bufcache_slot *bufcache_pick_victim(void)
{
	struct bufcache_slot *victim = NULL;

	for (size_t i = 0; i < BUFCACHE_SLOTS; i++) {
		/* Never evict a slot that is currently loading. */
		if (slots[i].loading)
			continue;
		if (!slots[i].valid && slots[i].buf.refcount == 0)
			return &slots[i];
		if (slots[i].buf.refcount != 0 || slots[i].buf.dirty)
			continue;
		if (!victim || slots[i].lru < victim->lru)
			victim = &slots[i];
	}

	return victim;
}

/*
 * Return a referenced cache buffer for a MINIX block.
 *
 * Parameters:
 *   bdev   block device that owns the block.
 *   block  1 KiB MINIX block number.
 *
 * Returns:
 *   Referenced buffer on success, NULL on I/O failure or cache pressure.
 *
 * Locking:
 *   Takes bufcache_lock internally.
 */
struct minix_buffer *bufcache_get(struct block_device *bdev, uint32_t block)
{
	struct bufcache_slot *slot;
	int err;

	if (!bdev || bdev->sector_size != 512)
		return NULL;

retry:
	mutex_lock(&bufcache_mutex);
	for (size_t i = 0; i < BUFCACHE_SLOTS; i++) {
		if (slots[i].buf.bdev != bdev || slots[i].buf.block_no != block)
			continue;
		if (!slots[i].valid && !slots[i].loading)
			continue;
		if (slots[i].loading) {
			/*
			 * v0.0.3.1 audit item 3: another thread is
			 * loading this exact (bdev, block) right now.
			 * Wait for it to finish instead of picking a
			 * fresh victim and creating a duplicate slot.
			 * Single-CPU + IRQ-disable means this branch
			 * is currently unreachable, but the structure
			 * is in place for SMP.
			 */
			mutex_unlock(&bufcache_mutex);
			sched_yield();
			goto retry;
		}

		slots[i].buf.refcount++;
		slots[i].lru = ++bufcache_clock;
		bufcache_hits++;
		mutex_unlock(&bufcache_mutex);
		return &slots[i].buf;
	}

	bufcache_misses++;
	slot = bufcache_pick_victim();
	if (!slot) {
		mutex_unlock(&bufcache_mutex);
		pr_err("bufcache: no clean unreferenced slot for block %u\n",
		       block);
		return NULL;
	}

	if (slot->valid && slot->buf.dirty && bufcache_dirty_count > 0)
		bufcache_dirty_count--;
	slot->valid = false;
	slot->loading = true; /* publish (bdev, block) before dropping lock */
	slot->buf.bdev = bdev;
	slot->buf.block_no = block;
	slot->buf.refcount = 1;
	slot->buf.dirty = false;
	slot->lru = ++bufcache_clock;
	mutex_unlock(&bufcache_mutex);

	err = block_read(bdev, (uint64_t)block * BUFCACHE_SECTORS_PER_BLOCK,
			 BUFCACHE_SECTORS_PER_BLOCK, slot->buf.data);
	if (err) {
		mutex_lock(&bufcache_mutex);
		slot->valid = false;
		slot->loading = false;
		slot->buf.refcount = 0;
		mutex_unlock(&bufcache_mutex);
		return NULL;
	}

	mutex_lock(&bufcache_mutex);
	slot->valid = true;
	slot->loading = false;
	mutex_unlock(&bufcache_mutex);
	return &slot->buf;
}

/*
 * Drop a cache buffer reference.
 *
 * Parameters:
 *   buf  buffer returned by bufcache_get().
 *
 * Locking:
 *   Takes bufcache_lock internally.
 */
void bufcache_put(struct minix_buffer *buf)
{
	if (!buf)
		return;

	mutex_lock(&bufcache_mutex);
	if (buf->refcount == 0) {
		mutex_unlock(&bufcache_mutex);
		pr_err("bufcache: put on unreferenced block %u\n",
		       buf->block_no);
		return;
	}
	buf->refcount--;
	mutex_unlock(&bufcache_mutex);
}

void bufcache_mark_dirty(struct minix_buffer *buf)
{
	if (!buf)
		return;

	mutex_lock(&bufcache_mutex);
	if (!buf->dirty && bufcache_dirty_count >= BUFCACHE_DIRTY_LIMIT) {
		struct block_device *bdev = buf->bdev;

		mutex_unlock(&bufcache_mutex);
		(void)bufcache_sync(bdev);
		mutex_lock(&bufcache_mutex);
	}
	if (!buf->dirty)
		bufcache_dirty_count++;
	buf->dirty = true;
	mutex_unlock(&bufcache_mutex);
}

/*
 * Synchronize dirty buffers for one device.
 *
 * Chunk 1 has no writeback path, but the symbol exists so chunk 2 can
 * fill in dirty-buffer handling without changing callers.
 */
int bufcache_sync(struct block_device *bdev)
{
	for (size_t i = 0; i < BUFCACHE_SLOTS; i++) {
		struct block_device *slot_bdev;
		uint32_t block;
		bool do_write;
		int err;

		mutex_lock(&bufcache_mutex);
		slot_bdev = slots[i].buf.bdev;
		block = slots[i].buf.block_no;
		do_write = slots[i].valid && slots[i].buf.dirty &&
			   (!bdev || slot_bdev == bdev);
		if (do_write)
			slots[i].buf.refcount++;
		mutex_unlock(&bufcache_mutex);

		if (!do_write)
			continue;

		err = block_write(
		    slot_bdev, (uint64_t)block * BUFCACHE_SECTORS_PER_BLOCK,
		    BUFCACHE_SECTORS_PER_BLOCK, slots[i].buf.data);

		mutex_lock(&bufcache_mutex);
		if (!err) {
			if (slots[i].buf.dirty && bufcache_dirty_count > 0)
				bufcache_dirty_count--;
			slots[i].buf.dirty = false;
		}
		if (slots[i].buf.refcount > 0)
			slots[i].buf.refcount--;
		mutex_unlock(&bufcache_mutex);

		if (err)
			return err;
	}

	return 0;
}

/*
 * Flush the entire buffer cache: sync all dirty buffers, then
 * invalidate every unreferenced slot.  Used by the selftest to
 * guarantee a clean 64-slot cache regardless of prior I/O.
 */
void bufcache_flush_all(void)
{
	/* First sync all dirty buffers to their devices. */
	(void)bufcache_sync(NULL);

	/* Then invalidate every unreferenced, non-loading slot. */
	mutex_lock(&bufcache_mutex);
	for (size_t i = 0; i < BUFCACHE_SLOTS; i++) {
		if (slots[i].buf.refcount != 0 || slots[i].loading)
			continue;
		if (slots[i].valid && slots[i].buf.dirty &&
		    bufcache_dirty_count > 0)
			bufcache_dirty_count--;
		slots[i].valid = false;
		slots[i].buf.dirty = false;
		slots[i].buf.bdev = NULL;
		slots[i].buf.block_no = 0;
	}
	mutex_unlock(&bufcache_mutex);
}

void bufcache_log_stats(void)
{
	uint64_t hits;
	uint64_t misses;
	uint64_t total;
	uint64_t hit_pct;
	uint32_t dirty;

	/*
	 * v0.0.3.1 audit item 9: snapshot all counters under the
	 * lock so the printed numbers are mutually consistent and
	 * SMP-safe.  Reading hits/misses lockless is fine on a
	 * single CPU but races on SMP and can produce nonsense
	 * hit_rate values when one counter is observed mid-update.
	 */
	mutex_lock(&bufcache_mutex);
	hits = bufcache_hits;
	misses = bufcache_misses;
	dirty = bufcache_dirty_count;
	mutex_unlock(&bufcache_mutex);

	total = hits + misses;
	hit_pct = total ? (hits * 100u) / total : 100u;

	pr_info("bufcache: hits=%llu misses=%llu hit_rate=%llu%% dirty=%u/%u\n",
		(unsigned long long)hits, (unsigned long long)misses,
		(unsigned long long)hit_pct, (unsigned)dirty,
		(unsigned)BUFCACHE_SLOTS);
}

static uint8_t selftest_storage[BUFCACHE_SELFTEST_BLOCKS][MINIX_BLOCK_SIZE];

static int bufcache_selftest_read(struct block_device *bdev, uint64_t lba,
				  size_t count, void *buf)
{
	uint64_t block;

	(void)bdev;
	if (count != BUFCACHE_SECTORS_PER_BLOCK)
		return -EINVAL;
	if ((lba % BUFCACHE_SECTORS_PER_BLOCK) != 0)
		return -EINVAL;

	block = lba / BUFCACHE_SECTORS_PER_BLOCK;
	if (block >= BUFCACHE_SELFTEST_BLOCKS)
		return -EINVAL;

	memcpy(buf, selftest_storage[block], MINIX_BLOCK_SIZE);
	return 0;
}

static int bufcache_selftest_write(struct block_device *bdev, uint64_t lba,
				   size_t count, const void *buf)
{
	(void)bdev;
	(void)lba;
	(void)count;
	(void)buf;
	return -ENOSYS;
}

static const struct block_ops bufcache_selftest_ops = {
    .read = bufcache_selftest_read,
    .write = bufcache_selftest_write,
};

static struct block_device bufcache_selftest_bdev = {
    .name = "bufcache-test",
    .sector_size = 512,
    .sector_count = BUFCACHE_SELFTEST_BLOCKS * BUFCACHE_SECTORS_PER_BLOCK,
    .ops = &bufcache_selftest_ops,
};

int bufcache_selftest(void)
{
	struct minix_buffer *held;
	struct minix_buffer *buf;

	/*
	 * Flush any dirty/cached buffers left over from real filesystem
	 * operations so the selftest has all 64 slots available.
	 */
	bufcache_flush_all();

	for (size_t i = 0; i < BUFCACHE_SELFTEST_BLOCKS; i++) {
		for (size_t j = 0; j < MINIX_BLOCK_SIZE; j++)
			selftest_storage[i][j] = (uint8_t)(i ^ j);
	}

	buf = bufcache_get(&bufcache_selftest_bdev, 3);
	if (!buf)
		return -EIO;
	if (buf->data[17] != selftest_storage[3][17]) {
		bufcache_put(buf);
		return -EIO;
	}
	bufcache_put(buf);

	for (uint32_t i = 0; i < BUFCACHE_SLOTS + 1; i++) {
		buf = bufcache_get(&bufcache_selftest_bdev, i);
		if (!buf)
			return -EIO;
		if (buf->data[31] != selftest_storage[i][31]) {
			bufcache_put(buf);
			return -EIO;
		}
		bufcache_put(buf);
	}

	held = bufcache_get(&bufcache_selftest_bdev, 1);
	if (!held)
		return -EIO;
	for (uint32_t i = 2; i < BUFCACHE_SLOTS + 2; i++) {
		buf = bufcache_get(&bufcache_selftest_bdev, i);
		if (!buf) {
			bufcache_put(held);
			return -EIO;
		}
		bufcache_put(buf);
	}
	if (held->block_no != 1 || held->data[63] != selftest_storage[1][63]) {
		bufcache_put(held);
		return -EIO;
	}
	bufcache_put(held);

	if (bufcache_dirty_count != 0)
		return -EIO;

	/*
	 * Clean up selftest slots so subsequent tests (minix_write,
	 * minix_dir, etc.) get a pristine cache.
	 */
	bufcache_flush_all();

	pr_info("bufcache_selftest: [ OK ]\n");
	return 0;
}
