/*
 * kernel/syscall/sys_futex.c — futex(2) syscall.
 *
 * v0.0.5 (item 2: futex). Decodes the Linux x86_64 futex ABI and
 * forwards to the in-kernel primitives in kernel/kernel/futex.c.
 *
 * Argument shape (matching Linux):
 *   futex(uaddr, op, val, timeout_or_val2, uaddr2, val3)
 *
 * For FUTEX_WAIT the fourth argument is a pointer to a *relative*
 * struct timespec (NULL == wait forever). For FUTEX_REQUEUE it is
 * reinterpreted as val2 (the number of waiters to requeue). The
 * FUTEX_PRIVATE and FUTEX_CLOCK_REALTIME flag bits are masked off:
 * JNU has a single address space per thread group and one time source.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/base/types.h>
#include <jnu/kernel/futex.h>
#include <jnu/user/syscall.h>
#include <jnu/user/usercopy.h>
#include <uapi/jnu/errno.h>
#include <uapi/jnu/futex.h>

#define FUTEX_TIMEOUT_MAX_SEC ((~0ULL - 999999ULL) / 1000000ULL)

struct timespec {
	int64_t tv_sec;
	int64_t tv_nsec;
};

int64_t sys_futex(uint32_t *uaddr, int op, uint32_t val,
		  uint64_t timeout_or_val2, uint32_t *uaddr2, uint32_t val3)
{
	int cmd = op & FUTEX_CMD_MASK;

	(void)val3;

	switch (cmd) {
	case FUTEX_WAIT: {
		uint64_t timeout_us = 0;

		if (timeout_or_val2) {
			struct timespec ts;
			int err = copy_from_user(
			    &ts, (const void *)(uintptr_t)timeout_or_val2,
			    sizeof(ts));
			if (err) {
				return err;
			}
			if (ts.tv_sec < 0 || ts.tv_nsec < 0 ||
			    ts.tv_nsec >= 1000000000ll) {
				return -EINVAL;
			}
			if ((uint64_t)ts.tv_sec > FUTEX_TIMEOUT_MAX_SEC) {
				return -EINVAL;
			}
			timeout_us = (uint64_t)ts.tv_sec * 1000000ull +
				     (uint64_t)ts.tv_nsec / 1000ull;
			/*
			 * A zero microsecond budget would otherwise be read
			 * as "wait forever". Treat a sub-microsecond
			 * timeout as the smallest finite wait.
			 */
			if (timeout_us == 0) {
				timeout_us = 1;
			}
		}
		return futex_wait(uaddr, val, timeout_us);
	}
	case FUTEX_WAKE:
		return futex_wake(uaddr, (int)val);
	case FUTEX_REQUEUE:
		if ((uintptr_t)uaddr2 & 0x3u) {
			return -EINVAL;
		}
		if (!user_range_ok(uaddr2, sizeof(*uaddr2))) {
			return -EFAULT;
		}
		return futex_requeue(uaddr, (int)val, (int)timeout_or_val2);
	default:
		return -ENOSYS;
	}
}
