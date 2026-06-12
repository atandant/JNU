/*
 * kernel/syscall/sys_exit_group.c — exit_group syscall.
 *
 * v0.0.4: terminate the entire thread group. Unlike exit() (which
 * retires only the calling thread), exit_group() flags every sibling
 * thread with TIF_NEED_DIE so they unwind and retire at their next
 * return-to-user boundary, then exits the caller. musl's _Exit() and
 * abort() route here.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/base/types.h>
#include <jnu/kernel/process.h>
#include <jnu/user/syscall.h>

int64_t sys_exit_group(int status)
{
	process_group_exit(status);
	return 0;
}
