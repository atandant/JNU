/*
 * kernel/syscall/sys_nanosleep.c — nanosleep syscall.
 *
 * v0.0.3 §2.9: TSC-based busy-yield.  Calls sched_yield() in a loop
 * until the requested deadline elapses.  No high-resolution timers
 * are available; this is a best-effort sleep.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/arch/cpu.h>
#include <jnu/base/types.h>
#include <jnu/kernel/sched.h>
#include <jnu/user/syscall.h>
#include <jnu/user/usercopy.h>
#include <uapi/jnu/errno.h>

struct timespec {
	int64_t tv_sec;
	int64_t tv_nsec;
};

int64_t sys_nanosleep(const void *ureq, void *urem)
{
	struct timespec req;
	uint64_t us_target;
	uint64_t start;
	int err;

	(void)urem;

	err = copy_from_user(&req, ureq, sizeof(req));
	if (err) {
		return (int64_t)err;
	}

	if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000ll) {
		return -EINVAL;
	}

	/* Convert to microseconds for TSC comparison. */
	us_target =
	    (uint64_t)req.tv_sec * 1000000ull + (uint64_t)req.tv_nsec / 1000ull;

	if (us_target == 0) {
		return 0;
	}

	start = cpu_us_since_boot();
	while (cpu_us_since_boot() - start < us_target) {
		sched_yield();
	}

	return 0;
}
