/*
 * kernel/kernel/fork.c — process_fork().
 *
 * Phase 2 of v0.0.2.1. Builds a child process by deep-cloning the
 * caller's address space (vmm_clone_space) and fd table
 * (fd_table_clone), allocating a fresh pid + kernel task, and forging
 * the child's first userspace return so it resumes at the parent's
 * post-syscall RIP/RSP with rax = 0.
 *
 * The child's first userspace entry is achieved without a custom
 * trampoline: sched_create_user_task already arranges
 * `user_thread_entry` to call `usermode_enter(proc->user_entry,
 * proc->user_stack)` on first scheduling. We simply set those fields
 * to the parent's saved RIP/RSP. usermode_enter zeroes every general
 * register before iretq, so the child wakes with rax = 0 by
 * construction — which is the fork ABI's child-return contract.
 *
 * v0.0.2.1 ships full-copy fork. CoW is v0.0.2.2 per jnuspec021.md
 * §2.1.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/fd.h>
#include <jnu/klog.h>
#include <jnu/kmalloc.h>
#include <jnu/process.h>
#include <jnu/sched.h>
#include <jnu/spinlock.h>
#include <jnu/types.h>
#include <jnu/vmm.h>

extern struct spinlock process_tree_lock;

int process_fork(uint64_t user_rip, uint64_t user_rsp, int *pid_out)
{
	struct task *parent_task = sched_current();
	struct process *parent;
	struct process *child;
	struct addr_space *child_space = NULL;
	int err;

	if (!pid_out || !parent_task || !parent_task->process) {
		return -EINVAL;
	}
	if (!user_rip || !user_rsp) {
		return -EINVAL;
	}

	parent = parent_task->process;

	child = kzalloc(sizeof(*child));
	if (!child) {
		return -ENOMEM;
	}

	child->pid = process_alloc_pid();
	if (child->pid < 0) {
		err = child->pid;
		goto fail_child;
	}
	child->state = PROCESS_ALIVE;
	child->parent = parent;

	/*
	 * Address space first: clone failure is by far the most likely
	 * out-of-memory path, and freeing the empty pid + struct
	 * process is cheaper than unwinding a half-built fd table.
	 */
	err = vmm_clone_space(parent->space, &child_space);
	if (err) {
		goto fail_pid;
	}
	child->space = child_space;

	fd_table_init(&child->fds);
	fd_table_clone(&child->fds, &parent->fds);

	/*
	 * Forge the child's first userspace return: when the scheduler
	 * picks up the child task, user_thread_entry calls
	 * usermode_enter(child->user_entry, child->user_stack), which
	 * iretqs into ring 3 with all GP regs zero (including rax).
	 * The userspace fork() wrapper sees rax = 0 and returns 0 to
	 * the caller in the child; the parent path returns child->pid.
	 */
	child->user_entry = user_rip;
	child->user_stack = user_rsp;

	err = sched_create_user_task("fork", child, NULL);
	if (err) {
		goto fail_fds;
	}

	{
		uint64_t flags = spin_lock_irqsave(&process_tree_lock);

		child->next_sibling = parent->first_child;
		parent->first_child = child;
		spin_unlock_irqrestore(&process_tree_lock, flags);
	}

	*pid_out = child->pid;
	return 0;

fail_fds:
	for (int fd = 0; fd < JNU_MAX_FDS; fd++) {
		file_put(fd_close(&child->fds, fd));
	}
	vmm_destroy_space(child->space);
fail_pid:
	process_release_pid(child->pid);
fail_child:
	kfree(child);
	return err;
}
