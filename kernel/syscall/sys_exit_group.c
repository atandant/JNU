/*
 * kernel/syscall/sys_exit_group.c — exit_group syscall.
 *
 * v0.0.3 §2.9: alias of exit (60).  JNU has no threads yet, so
 * exit_group and exit are semantically identical.  musl's _Exit()
 * calls exit_group rather than exit.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/syscall.h>
#include <jnu/types.h>

int64_t sys_exit_group(int status)
{
	return sys_exit(status);
}
