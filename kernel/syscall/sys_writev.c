/*
 * kernel/syscall/sys_writev.c — writev syscall.
 *
 * v0.0.3 §2.9: walks the user-provided iovec array, calling the
 * existing sys_write path for each entry.  Returns total bytes
 * written.  musl's printf implementation uses writev for buffered
 * output flushing.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/syscall.h>
#include <jnu/types.h>
#include <jnu/usercopy.h>

/*
 * Linux-compatible iovec.  musl passes this layout directly from
 * userspace.
 */
struct iovec {
	void *iov_base;
	size_t iov_len;
};

#define UIO_MAXIOV 1024

int64_t sys_writev(int fd, const void *uiov, int iovcnt)
{
	struct iovec iov;
	int64_t total = 0;

	if (iovcnt <= 0 || iovcnt > UIO_MAXIOV) {
		return -EINVAL;
	}

	for (int i = 0; i < iovcnt; i++) {
		const struct iovec *entry =
		    (const struct iovec *)uiov + i;
		int err;
		int64_t n;

		err = copy_from_user(&iov, entry, sizeof(iov));
		if (err) {
			return total > 0 ? total : (int64_t)err;
		}

		if (iov.iov_len == 0) {
			continue;
		}

		n = sys_write(fd, iov.iov_base, iov.iov_len);
		if (n < 0) {
			return total > 0 ? total : n;
		}
		total += n;

		/* Short write — stop iterating, return partial. */
		if ((size_t)n < iov.iov_len) {
			break;
		}
	}

	return total;
}
