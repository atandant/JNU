/*
 * kernel/syscall/sys_clone.c — clone() syscall (thread creation).
 *
 * v0.0.4 (item 1: threads). Supports the thread-creation subset of
 * clone() that musl's pthread_create() uses: CLONE_VM | CLONE_THREAD
 * plus the TLS/tid helper flags. The new task shares the caller's
 * address space, fd table and pid (tgid), gets a fresh tid, and resumes
 * in userspace on `child_stack` with rax = 0.
 *
 * x86_64 register ABI:
 *   clone(flags, child_stack, parent_tid, child_tid, tls)
 *   = rdi(arg0), rsi(arg1), rdx(arg2), r10(arg3), r8(arg4)
 *
 * The child does not "return" through this path: like fork's child it
 * wakes directly in userspace via the forged frame. The parent receives
 * the new tid.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/base/types.h>
#include <jnu/kernel/process.h>
#include <jnu/user/syscall.h>
#include <uapi/jnu/errno.h>

int64_t sys_clone(const struct syscall_args *args)
{
	struct syscall_frame frame;
	uint64_t flags;
	uint64_t child_stack;
	void *parent_tid;
	void *child_tid;
	uint64_t tls;
	int tid;
	int err;

	if (!args) {
		return -EINVAL;
	}

	flags = args->arg0;
	child_stack = args->arg1;
	parent_tid = (void *)args->arg2;
	child_tid = (void *)args->arg3;
	tls = args->arg4;

	frame.args = *args;
	frame.user = *syscall_user_state_of(args);

	err = process_clone_thread(&frame, child_stack, tls, parent_tid,
				   child_tid, flags, &tid);
	if (err) {
		return err;
	}
	return tid;
}
