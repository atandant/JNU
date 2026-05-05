/*
 * kernel/fs/minix/file.c — MINIX v1 regular-file reads.
 *
 * Provides the read-only VFS file operation. Logical blocks are mapped
 * by inode.c and physical blocks are read through the buffer cache; no
 * filesystem code performs bare block-device I/O in chunk 1.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "internal.h"

#include <jnu/errno.h>
#include <jnu/klog.h>
#include <jnu/kmalloc.h>
#include <jnu/minix.h>
#include <jnu/rtc.h>
#include <jnu/string.h>

static void minix_clear_block_mapping(struct vfs_inode *ino, uint32_t lblk);
static void minix_prune_empty_indirects(struct vfs_inode *ino);

/*
 * Read bytes from a MINIX inode.
 *
 * Returns:
 *   Number of bytes read on success, negative errno on failure.
 */
ssize_t minix_read(struct vfs_inode *ino, uint64_t offset, size_t len,
		   void *buf)
{
	struct minix_inode_info *mi = ino->priv;
	uint8_t *p = buf;
	size_t left;
	uint64_t curr_off;

	if (offset >= ino->size)
		return 0;
	if (len > (size_t)(ino->size - offset))
		len = (size_t)(ino->size - offset);

	left = len;
	curr_off = offset;

	while (left > 0) {
		uint32_t lblk = (uint32_t)(curr_off / MINIX_BLOCK_SIZE);
		uint32_t boff = (uint32_t)(curr_off % MINIX_BLOCK_SIZE);
		uint32_t pblk = minix_bmap(ino->mnt, mi, lblk, false);
		uint32_t chunk = MINIX_BLOCK_SIZE - boff;

		if (chunk > left)
			chunk = (uint32_t)left;

		if (pblk == 0) {
			memset(p, 0, chunk);
		} else {
			struct minix_buffer *mbuf;

			mbuf = bufcache_get(ino->mnt->bdev, pblk);
			if (!mbuf)
				return -EIO;
			memcpy(p, mbuf->data + boff, chunk);
			bufcache_put(mbuf);
		}

		p += chunk;
		left -= chunk;
		curr_off += chunk;
	}

	return (ssize_t)len;
}

ssize_t minix_write(struct vfs_inode *ino, uint64_t offset, size_t len,
		    const void *buf)
{
	struct minix_inode_info *mi = ino->priv;
	const uint8_t *p = buf;
	uint8_t *backup;
	uint32_t *created;
	uint32_t created_count = 0;
	uint32_t first_lblk;
	uint32_t last_lblk;
	uint32_t block_count;
	uint64_t end;
	uint64_t old_size = ino->size;
	uint32_t old_time = mi->raw.i_time;
	size_t left = len;
	uint64_t curr_off = offset;
	int err = 0;

	if (len == 0)
		return 0;
	if (__builtin_add_overflow(offset, (uint64_t)len, &end))
		return -EINVAL;
	if (end > 0xffffffffu)
		return -EINVAL;

	first_lblk = (uint32_t)(offset / MINIX_BLOCK_SIZE);
	last_lblk = (uint32_t)((end - 1u) / MINIX_BLOCK_SIZE);
	block_count = last_lblk - first_lblk + 1u;

	created = kmalloc((size_t)block_count * sizeof(*created));
	if (!created)
		return -ENOMEM;
	backup = kmalloc(len);
	if (!backup) {
		kfree(created);
		return -ENOMEM;
	}
	memset(backup, 0, len);
	if (offset < old_size) {
		size_t read_len = len;
		ssize_t n;

		if (read_len > (size_t)(old_size - offset))
			read_len = (size_t)(old_size - offset);
		n = minix_read(ino, offset, read_len, backup);
		if (n != (ssize_t)read_len) {
			kfree(backup);
			kfree(created);
			return -EIO;
		}
	}

	for (uint32_t i = first_lblk; i <= last_lblk; i++) {
		bool missing = minix_bmap(ino->mnt, mi, i, false) == 0;
		uint32_t pblk = minix_bmap(ino->mnt, mi, i, true);

		if (pblk == 0) {
			err = -ENOSPC;
			goto rollback_alloc;
		}
		if (missing)
			created[created_count++] = i;
	}

	while (left > 0) {
		uint32_t lblk = (uint32_t)(curr_off / MINIX_BLOCK_SIZE);
		uint32_t boff = (uint32_t)(curr_off % MINIX_BLOCK_SIZE);
		uint32_t pblk = minix_bmap(ino->mnt, mi, lblk, false);
		uint32_t chunk = MINIX_BLOCK_SIZE - boff;
		struct minix_buffer *mbuf;

		if (chunk > left)
			chunk = (uint32_t)left;
		if (pblk == 0) {
			err = -EIO;
			goto rollback_data;
		}

		mbuf = bufcache_get(ino->mnt->bdev, pblk);
		if (!mbuf) {
			err = -EIO;
			goto rollback_data;
		}

		memcpy(mbuf->data + boff, p, chunk);
		bufcache_mark_dirty(mbuf);
		bufcache_put(mbuf);

		p += chunk;
		left -= chunk;
		curr_off += chunk;
	}

	if (offset + len > ino->size) {
		ino->size = offset + len;
		mi->raw.i_size = (uint32_t)ino->size;
	}
	mi->raw.i_time = rtc_now_unix();
	mi->dirty = true;
	kfree(backup);
	kfree(created);
	return (ssize_t)len;

rollback_data:
	left = len - left;
	curr_off = offset;
	while (left > 0) {
		uint32_t lblk = (uint32_t)(curr_off / MINIX_BLOCK_SIZE);
		uint32_t boff = (uint32_t)(curr_off % MINIX_BLOCK_SIZE);
		uint32_t pblk = minix_bmap(ino->mnt, mi, lblk, false);
		uint32_t chunk = MINIX_BLOCK_SIZE - boff;
		struct minix_buffer *mbuf;

		if (chunk > left)
			chunk = (uint32_t)left;
		if (pblk != 0) {
			mbuf = bufcache_get(ino->mnt->bdev, pblk);
			if (mbuf) {
				memcpy(mbuf->data + boff,
				       backup + (curr_off - offset), chunk);
				bufcache_mark_dirty(mbuf);
				bufcache_put(mbuf);
			}
		}
		left -= chunk;
		curr_off += chunk;
	}

rollback_alloc:
	for (uint32_t i = 0; i < created_count; i++)
		minix_clear_block_mapping(ino, created[i]);
	minix_prune_empty_indirects(ino);
	ino->size = old_size;
	mi->raw.i_size = (uint32_t)old_size;
	mi->raw.i_time = old_time;
	mi->dirty = true;
	kfree(backup);
	kfree(created);
	return err;
}

static void minix_free_indirect(struct vfs_mount *mnt, uint32_t zone)
{
	struct minix_buffer *buf;
	uint16_t *table;

	if (zone == 0)
		return;

	buf = bufcache_get(mnt->bdev, zone);
	if (!buf)
		return;
	table = (uint16_t *)buf->data;
	for (uint32_t i = 0; i < MINIX_BLOCK_SIZE / sizeof(uint16_t); i++) {
		if (table[i] != 0)
			minix_free_zone(mnt, table[i]);
	}
	bufcache_put(buf);
	minix_free_zone(mnt, zone);
}

static void minix_free_double_indirect(struct vfs_mount *mnt, uint32_t zone)
{
	struct minix_buffer *buf;
	uint16_t copy[MINIX_BLOCK_SIZE / sizeof(uint16_t)];

	if (zone == 0)
		return;

	buf = bufcache_get(mnt->bdev, zone);
	if (!buf)
		return;
	memcpy(copy, buf->data, sizeof(copy));
	bufcache_put(buf);

	for (uint32_t i = 0; i < MINIX_BLOCK_SIZE / sizeof(uint16_t); i++) {
		if (copy[i] != 0)
			minix_free_indirect(mnt, copy[i]);
	}
	minix_free_zone(mnt, zone);
}

static void minix_clear_block_mapping(struct vfs_inode *ino, uint32_t lblk)
{
	struct minix_inode_info *mi = ino->priv;
	uint16_t per_block = MINIX_BLOCK_SIZE / sizeof(uint16_t);
	struct minix_buffer *buf;

	if (lblk < 7) {
		if (mi->raw.i_zone[lblk] != 0) {
			minix_free_zone(ino->mnt, mi->raw.i_zone[lblk]);
			mi->raw.i_zone[lblk] = 0;
		}
		return;
	}
	lblk -= 7;
	if (lblk < per_block) {
		if (mi->raw.i_zone[7] == 0)
			return;
		buf = bufcache_get(ino->mnt->bdev, mi->raw.i_zone[7]);
		if (!buf)
			return;
		if (((uint16_t *)buf->data)[lblk] != 0) {
			minix_free_zone(ino->mnt,
					((uint16_t *)buf->data)[lblk]);
			((uint16_t *)buf->data)[lblk] = 0;
			bufcache_mark_dirty(buf);
		}
		bufcache_put(buf);
		return;
	}
	lblk -= per_block;
	if (mi->raw.i_zone[8] != 0) {
		uint32_t outer = lblk / per_block;
		uint32_t inner = lblk % per_block;
		uint16_t ind;

		buf = bufcache_get(ino->mnt->bdev, mi->raw.i_zone[8]);
		if (!buf)
			return;
		ind = ((uint16_t *)buf->data)[outer];
		bufcache_put(buf);
		if (ind == 0)
			return;

		buf = bufcache_get(ino->mnt->bdev, ind);
		if (!buf)
			return;
		if (((uint16_t *)buf->data)[inner] != 0) {
			minix_free_zone(ino->mnt,
					((uint16_t *)buf->data)[inner]);
			((uint16_t *)buf->data)[inner] = 0;
			bufcache_mark_dirty(buf);
		}
		bufcache_put(buf);
	}
}

static bool minix_indirect_empty(struct vfs_mount *mnt, uint32_t zone)
{
	struct minix_buffer *buf;
	bool empty = true;

	if (zone == 0)
		return true;
	buf = bufcache_get(mnt->bdev, zone);
	if (!buf)
		return false;
	for (uint32_t i = 0; i < MINIX_BLOCK_SIZE / sizeof(uint16_t); i++) {
		if (((uint16_t *)buf->data)[i] != 0) {
			empty = false;
			break;
		}
	}
	bufcache_put(buf);
	return empty;
}

static void minix_prune_empty_indirects(struct vfs_inode *ino)
{
	struct minix_inode_info *mi = ino->priv;
	struct minix_buffer *buf;

	if (mi->raw.i_zone[7] != 0 &&
	    minix_indirect_empty(ino->mnt, mi->raw.i_zone[7])) {
		minix_free_zone(ino->mnt, mi->raw.i_zone[7]);
		mi->raw.i_zone[7] = 0;
	}
	if (mi->raw.i_zone[8] == 0)
		return;

	buf = bufcache_get(ino->mnt->bdev, mi->raw.i_zone[8]);
	if (!buf)
		return;
	for (uint32_t i = 0; i < MINIX_BLOCK_SIZE / sizeof(uint16_t); i++) {
		uint16_t ind = ((uint16_t *)buf->data)[i];

		if (ind != 0 && minix_indirect_empty(ino->mnt, ind)) {
			minix_free_zone(ino->mnt, ind);
			((uint16_t *)buf->data)[i] = 0;
			bufcache_mark_dirty(buf);
		}
	}
	bufcache_put(buf);
	if (minix_indirect_empty(ino->mnt, mi->raw.i_zone[8])) {
		minix_free_zone(ino->mnt, mi->raw.i_zone[8]);
		mi->raw.i_zone[8] = 0;
	}
}

int minix_truncate(struct vfs_inode *ino, uint64_t size)
{
	struct minix_inode_info *mi = ino->priv;
	uint32_t keep_blocks;
	uint32_t old_blocks;

	if (size > 0 && size > ino->size) {
		ino->size = size;
		mi->raw.i_size = (uint32_t)size;
		mi->raw.i_time = rtc_now_unix();
		mi->dirty = true;
		return 0;
	}
	if (size == ino->size)
		return 0;

	keep_blocks =
	    (uint32_t)((size + MINIX_BLOCK_SIZE - 1) / MINIX_BLOCK_SIZE);
	if (keep_blocks != 0) {
		old_blocks = (uint32_t)((ino->size + MINIX_BLOCK_SIZE - 1) /
					MINIX_BLOCK_SIZE);
		for (uint32_t i = keep_blocks; i < old_blocks; i++)
			minix_clear_block_mapping(ino, i);
		minix_prune_empty_indirects(ino);
		ino->size = size;
		mi->raw.i_size = (uint32_t)size;
		mi->raw.i_time = rtc_now_unix();
		mi->dirty = true;
		return 0;
	}

	for (uint32_t i = 0; i < 7; i++) {
		if (mi->raw.i_zone[i] != 0) {
			minix_free_zone(ino->mnt, mi->raw.i_zone[i]);
			mi->raw.i_zone[i] = 0;
		}
	}
	minix_free_indirect(ino->mnt, mi->raw.i_zone[7]);
	minix_free_double_indirect(ino->mnt, mi->raw.i_zone[8]);
	mi->raw.i_zone[7] = 0;
	mi->raw.i_zone[8] = 0;
	ino->size = size;
	mi->raw.i_size = (uint32_t)size;
	mi->raw.i_time = rtc_now_unix();
	mi->dirty = true;
	return 0;
}

int minix_write_selftest(void)
{
	struct vfs_inode *ino;
	char out[32];
	const char msg[] = "chunk2-write";
	int err;
	ssize_t n;

	(void)vfs_unlink("/c2-write");
	err = vfs_create("/c2-write", 0666, &ino);
	if (err) {
		pr_err("minix_write_selftest: create failed err=%d\n", err);
		return err;
	}
	n = vfs_write(ino, 0, sizeof(msg), msg);
	if (n != (ssize_t)sizeof(msg)) {
		vfs_close(ino);
		return -EIO;
	}
	err = vfs_fsync(ino);
	if (err) {
		vfs_close(ino);
		return err;
	}
	vfs_close(ino);

	err = vfs_open("/c2-write", &ino);
	if (err) {
		pr_err("minix_write_selftest: reopen failed err=%d\n", err);
		return err;
	}
	memset(out, 0, sizeof(out));
	n = vfs_read(ino, 0, sizeof(out), out);
	if (n < (ssize_t)sizeof(msg) || memcmp(out, msg, sizeof(msg)) != 0) {
		vfs_close(ino);
		return -EIO;
	}
	err = vfs_truncate(ino, 5);
	if (err) {
		vfs_close(ino);
		return err;
	}
	if (ino->size != 5) {
		vfs_close(ino);
		return -EIO;
	}
	err = vfs_truncate(ino, 4096);
	if (err) {
		vfs_close(ino);
		return err;
	}
	if (ino->size != 4096) {
		vfs_close(ino);
		return -EIO;
	}
	vfs_close(ino);
	err = vfs_unlink("/c2-write");
	if (err)
		return err;

	pr_info("minix_write_selftest: [ OK ]\n");
	return 0;
}
