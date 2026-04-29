/*
 * include/jnu/block.h — Block device abstraction layer.
 *
 * A block device has a name, a sector size, and a sector count.
 * v0.0.1 supports only read; write is a no-op placeholder. Devices
 * register themselves and can be looked up by name for filesystem
 * mounting.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>

struct block_device;

struct block_ops {
	/*
	 * Read `count` sectors starting at LBA `lba` into `buf`.
	 * Returns 0 on success, negative errno on failure.
	 */
	int (*read)(struct block_device *bdev, uint64_t lba,
		    size_t count, void *buf);

	/*
	 * Write `count` sectors starting at LBA `lba` from `buf`.
	 * v0.0.1: always returns -ENOSYS.
	 */
	int (*write)(struct block_device *bdev, uint64_t lba,
		     size_t count, const void *buf);
};

struct block_device {
	const char		*name;		/* e.g. "hda" */
	uint32_t		sector_size;	/* typically 512 */
	uint64_t		sector_count;
	const struct block_ops	*ops;
	void			*priv;		/* driver-private data */
};

/*
 * Register a block device. The caller retains ownership of the struct;
 * the registry stores a pointer. Returns 0 on success or -ENOMEM/-EEXIST.
 */
int block_register(struct block_device *bdev);

/*
 * Look up a registered block device by name. Returns NULL if not found.
 */
struct block_device *block_lookup(const char *name);

/*
 * Read `count` sectors from a block device. Convenience wrapper that
 * calls through bdev->ops->read.
 */
int block_read(struct block_device *bdev, uint64_t lba,
	       size_t count, void *buf);
