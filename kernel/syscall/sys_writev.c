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

#include <jnu/base/types.h>
#include <jnu/user/syscall.h>
#include <jnu/user/usercopy.h>
#include <uapi/jnu/errno.h>

/*
 * Linux-compatible iovec.  musl passes this layout directly from
 * userspace.
 */
struct iovec {
	void *iov_base;
	size_t iov_len;
};

#include <jnu/mm/kmalloc.h>

#define UIO_MAXIOV 1024

int64_t sys_writev(int fd, const void *uiov, int iovcnt)
{
	struct iovec *iov;
	int64_t total = 0;
	size_t total_len = 0;
	int err;

	if (iovcnt <= 0 || iovcnt > UIO_MAXIOV) {
		return -EINVAL;
	}

	iov = kmalloc((size_t)iovcnt * sizeof(struct iovec));
	if (!iov) {
		return -ENOMEM;
	}

	err = copy_from_user(iov, uiov, (size_t)iovcnt * sizeof(struct iovec));
	if (err) {
		kfree(iov);
		return err;
	}

	/* Pre-validate total length to prevent ssize_t overflow (POSIX
	 * requirement) */
	for (int i = 0; i < iovcnt; i++) {
		if (__builtin_add_overflow(total_len, iov[i].iov_len,
					   &total_len) ||
		    total_len > (size_t)0x7FFFFFFFFFFFFFFFull) {
			kfree(iov);
			return -EINVAL;
		}
	}

	for (int i = 0; i < iovcnt; i++) {
		int64_t n;

		if (iov[i].iov_len == 0) {
			continue;
		}

		n = sys_write(fd, iov[i].iov_base, iov[i].iov_len);
		if (n < 0) {
			kfree(iov);
			return total > 0 ? total : n;
		}

		total += n;

		/* Short write — stop iterating, return partial. */
		if ((size_t)n < iov[i].iov_len) {
			break;
		}
	}

	kfree(iov);
	return total;
}
