/*
 * kernel/syscall/sys_getpid.c - getpid syscall.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/sched.h>
#include <jnu/syscall.h>

int64_t sys_getpid(void)
{
	struct task *task = sched_current();

	return task ? task->pid : 0;
}
