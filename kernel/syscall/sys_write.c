/*
 * kernel/syscall/sys_write.c - write syscall.
 *
 * fd 1 (stdout) and fd 2 (stderr) are routed through the regular klog
 * pipeline, NOT klog_panic_write.  The panic path is documented in
 * jnuspec.md §2.11 as "no locks taken, no allocation, no ring-buffer
 * queueing - direct write to backends"; it must remain reachable only
 * from panic() so that the forensic dump is not interleaved with
 * concurrent userspace writes and so userspace cannot bypass logging
 * locks or the ring buffer.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/klog.h>
#include <jnu/string.h>
#include <jnu/syscall.h>
#include <jnu/usercopy.h>

#define WRITE_CHUNK 128

int64_t sys_write(int fd, const void *ubuf, size_t len)
{
	char buf[WRITE_CHUNK + 1];
	enum klog_level level;
	size_t done = 0;

	if (fd == 1) {
		level = KLOG_INFO;
	} else if (fd == 2) {
		level = KLOG_ERR;
	} else {
		return -EINVAL;
	}

	while (done < len) {
		size_t chunk = len - done;
		int err;

		if (chunk > WRITE_CHUNK) {
			chunk = WRITE_CHUNK;
		}

		err = copy_from_user(buf, (const uint8_t *)ubuf + done, chunk);
		if (err) {
			return done ? (int64_t)done : err;
		}

		/*
		 * klog_raw_write goes through the normal locked ring-buffer
		 * path without adding kernel prefixes or forced newlines.
		 * Using klog_panic_write here would race the panic channel
		 * and let userspace bypass logging locks.
		 */
		klog_raw_write(level, buf, chunk);
		done += chunk;
	}

	return (int64_t)done;
}
