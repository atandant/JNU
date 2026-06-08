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

#include <jnu/kernel/process.h>
#include <jnu/user/syscall.h>
#include <uapi/jnu/errno.h>

int64_t sys_fork(const struct syscall_args *args)
{
	struct syscall_frame frame;
	int pid;
	int err;

	if (!args) {
		return -EINVAL;
	}

	frame.args = *args;
	frame.user = *syscall_user_state_of(args);

	err = process_fork(&frame, &pid);
	if (err) {
		return err;
	}
	return pid;
}
