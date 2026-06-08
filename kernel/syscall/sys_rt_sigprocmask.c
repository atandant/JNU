/*
 * kernel/syscall/sys_rt_sigprocmask.c — rt_sigprocmask stub.
 *
 * v0.0.3 §2.9: returns 0 unconditionally.  No signals are delivered
 * in v0.0.3; this stub exists solely to prevent musl's static startup
 * from aborting on -ENOSYS.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/base/types.h>
#include <jnu/user/syscall.h>

int64_t sys_rt_sigprocmask(int how, const void *set, void *oldset,
			   size_t sigsetsize)
{
	(void)how;
	(void)set;
	(void)oldset;
	(void)sigsetsize;
	return 0;
}
