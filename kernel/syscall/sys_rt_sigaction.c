/*
 * kernel/syscall/sys_rt_sigaction.c — rt_sigaction stub.
 *
 * v0.0.3 §2.9: returns 0 unconditionally.  No signals are delivered
 * in v0.0.3; this stub exists solely to prevent musl's static startup
 * from aborting on -ENOSYS.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/syscall.h>
#include <jnu/types.h>

int64_t sys_rt_sigaction(int signum, const void *act, void *oldact,
			 size_t sigsetsize)
{
	(void)signum;
	(void)act;
	(void)oldact;
	(void)sigsetsize;
	return 0;
}
