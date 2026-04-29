/*
 * kernel/fs/block.c — Block device registry.
 *
 * Maintains a fixed-capacity table of registered block devices.
 * Drivers (ATA, future virtio-blk) call block_register() to add
 * themselves; filesystem code calls block_lookup() by name.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/block.h>
#include <jnu/errno.h>
#include <jnu/klog.h>
#include <jnu/string.h>
#include <jnu/types.h>

#define MAX_BLOCK_DEVICES	8

static struct block_device *bdevs[MAX_BLOCK_DEVICES];
static size_t bdev_count;

int block_register(struct block_device *bdev)
{
	if (!bdev || !bdev->name || !bdev->ops)
		return -EINVAL;

	for (size_t i = 0; i < bdev_count; i++) {
		if (strcmp(bdevs[i]->name, bdev->name) == 0)
			return -EEXIST;
	}

	if (bdev_count >= MAX_BLOCK_DEVICES) {
		pr_warn("block: device table full, cannot register '%s'\n",
			bdev->name);
		return -ENOMEM;
	}

	bdevs[bdev_count++] = bdev;
	pr_info("block: registered '%s' (%llu sectors, %u bytes/sector)\n",
		bdev->name,
		(unsigned long long)bdev->sector_count,
		(unsigned)bdev->sector_size);
	return 0;
}

struct block_device *block_lookup(const char *name)
{
	for (size_t i = 0; i < bdev_count; i++) {
		if (strcmp(bdevs[i]->name, name) == 0)
			return bdevs[i];
	}
	return NULL;
}

int block_read(struct block_device *bdev, uint64_t lba,
	       size_t count, void *buf)
{
	if (!bdev || !bdev->ops || !bdev->ops->read)
		return -EINVAL;
	return bdev->ops->read(bdev, lba, count, buf);
}
