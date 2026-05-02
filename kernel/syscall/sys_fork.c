/*
 * kernel/syscall/sys_fork.c — fork() syscall.
 *
 * Phase 2 of v0.0.2.1. Pulls the parent's saved user RIP/RSP out of
 * the syscall_entry frame trailer, hands them to process_fork(), and
 * returns the child pid in the parent. The child does not return
 * through this path: it wakes up directly in userspace via the iretq
 * frame forged by usermode_enter().
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/process.h>
#include <jnu/syscall.h>

int64_t sys_fork(const struct syscall_args *args)
{
	const struct syscall_user_state *us;
	int pid;
	int err;

	if (!args) {
		return -EINVAL;
	}

	us = syscall_user_state_of(args);
	err = process_fork(us->rip, us->rsp, &pid);
	if (err) {
		return err;
	}
	return pid;
}
