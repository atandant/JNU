/*
 * kernel/syscall/sys_yield.c - yield syscall.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/kernel/sched.h>
#include <jnu/user/syscall.h>

int64_t sys_yield(void)
{
	sched_yield();
	return 0;
}
