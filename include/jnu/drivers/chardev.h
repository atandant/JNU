/*
 * include/jnu/drivers/chardev.h — Character device abstraction.
 *
 * Minimal interface for byte-stream devices (keyboard, serial).
 * v0.0.1 uses it for PS/2 keyboard line-buffered input and serial
 * output; a richer API arrives when userspace needs open/close/ioctl.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/base/types.h>

struct char_device;

struct char_ops {
	/*
	 * Read up to `len` bytes into `buf`. Returns number of bytes
	 * actually read, or a negative errno.
	 *
	 * `buf` MUST be a kernel pointer.  Char device implementations
	 * write directly into `buf` (often under a spinlock with IRQs
	 * disabled), so passing a userspace pointer here would bypass
	 * SMAP and risk faulting inside an IRQ-off critical section
	 * (see jnuspec2.md §2.3 / §2.8).  Syscall layers must drain
	 * into a kernel bounce buffer first and then copy_to_user().
	 */
	ssize_t (*read)(struct char_device *dev, void *buf, size_t len);

	/*
	 * Non-blocking poll: returns true if at least one byte is
	 * available for read.
	 */
	bool (*poll)(struct char_device *dev);
};

struct char_device {
	const char *name;
	const struct char_ops *ops;
};
