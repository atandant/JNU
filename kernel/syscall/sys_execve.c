/*
 * kernel/syscall/sys_execve.c - execve() syscall.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/kernel/exec.h>
#include <jnu/user/syscall.h>
#include <uapi/jnu/errno.h>

int64_t sys_execve(const struct syscall_args *args)
{
	struct syscall_args *saved_args;
	struct syscall_user_state *us;
	uint64_t entry;
	uint64_t stack;
	int err;

	if (!args) {
		return -EINVAL;
	}

	err =
	    process_execve((const char *)args->arg0, (char *const *)args->arg1,
			   (char *const *)args->arg2, &entry, &stack);
	if (err) {
		return err;
	}

	saved_args = (struct syscall_args *)args;
	saved_args->arg0 = 0;
	saved_args->arg1 = 0;
	saved_args->arg2 = 0;
	saved_args->arg3 = 0;
	saved_args->arg4 = 0;
	saved_args->arg5 = 0;

	us = (struct syscall_user_state *)syscall_user_state_of(args);
	us->rip = entry;
	us->rsp = stack;
	us->rflags = (us->rflags | (1ull << 9) | 2ull) & ~(1ull << 8);
	us->r12 = 0;
	us->rbx = 0;
	us->rbp = 0;
	us->r13 = 0;
	us->r14 = 0;
	us->r15 = 0;
	return 0;
}
