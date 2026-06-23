/*
 * kernel/syscall/sys_getrandom.c — getrandom syscall.
 *
 * Backed by the kernel CSPRNG (BLAKE2s entropy conditioning + ChaCha20
 * with fast key erasure; see kernel/lib/csprng.c).  Output is suitable
 * for userspace stack-protector canaries and key material.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/base/types.h>
#include <jnu/lib/csprng.h>
#include <jnu/user/syscall.h>
#include <jnu/user/usercopy.h>
#include <uapi/jnu/errno.h>

int64_t sys_getrandom(void *ubuf, size_t buflen, unsigned int flags)
{
	uint8_t chunk[64];
	size_t done = 0;

	(void)flags;

	if (!ubuf || buflen == 0) {
		return -EINVAL;
	}

	while (done < buflen) {
		size_t want = buflen - done;
		int err;

		if (want > sizeof(chunk)) {
			want = sizeof(chunk);
		}

		csprng_bytes(chunk, want);

		err = copy_to_user((uint8_t *)ubuf + done, chunk, want);
		if (err) {
			return done > 0 ? (int64_t)done : (int64_t)err;
		}
		done += want;
	}

	return (int64_t)done;
}
