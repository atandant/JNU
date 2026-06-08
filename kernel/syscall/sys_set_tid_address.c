/*
 * kernel/syscall/sys_set_tid_address.c — set_tid_address stub.
 *
 * v0.0.3 §2.9: returns the current task's PID (TID == PID; no threads
 * in v0.0.3).  musl calls this during __init_tp() to register a
 * tid-address pointer for robust futex cleanup.  We ignore the pointer
 * because futex is not implemented yet.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/base/types.h>
#include <jnu/kernel/process.h>
#include <jnu/kernel/sched.h>
#include <jnu/user/syscall.h>

int64_t sys_set_tid_address(void *tidptr)
{
	struct task *t = sched_current();

	(void)tidptr;

	if (!t || !t->process) {
		return 1;
	}
	return (int64_t)t->process->pid;
}
