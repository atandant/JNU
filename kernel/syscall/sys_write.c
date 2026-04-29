/*
 * kernel/syscall/sys_write.c - write syscall.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/klog.h>
#include <jnu/string.h>
#include <jnu/syscall.h>
#include <jnu/usercopy.h>

#define WRITE_CHUNK	128

int64_t sys_write(int fd, const void *ubuf, size_t len)
{
	char buf[WRITE_CHUNK];
	size_t done = 0;

	if (fd != 1 && fd != 2) {
		return -EINVAL;
	}

	while (done < len) {
		size_t chunk = len - done;
		int err;

		if (chunk > sizeof(buf)) {
			chunk = sizeof(buf);
		}

		err = copy_from_user(buf, (const uint8_t *)ubuf + done,
				     chunk);
		if (err) {
			return done ? (int64_t)done : err;
		}

		klog_panic_write(buf, chunk);
		done += chunk;
	}

	return (int64_t)done;
}
