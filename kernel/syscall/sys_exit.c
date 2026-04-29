/*
 * kernel/syscall/sys_exit.c - exit syscall.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/process.h>
#include <jnu/sched.h>
#include <jnu/syscall.h>

int64_t sys_exit(int status)
{
	process_exit_current(status);
	sched_exit_current(status);
	return 0;
}
