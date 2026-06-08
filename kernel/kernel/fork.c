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

#include <jnu/base/types.h>
#include <jnu/kernel/process.h>
#include <jnu/kernel/sched.h>
#include <jnu/lib/klog.h>
#include <jnu/lib/spinlock.h>
#include <jnu/mm/kmalloc.h>
#include <jnu/mm/vmm.h>
#include <jnu/user/fd.h>
#include <jnu/user/syscall.h>
#include <uapi/jnu/errno.h>

extern struct spinlock process_tree_lock;

int process_fork(const struct syscall_frame *frame, int *pid_out)
{
	struct task *parent_task = sched_current();
	struct process *parent;
	struct process *child;
	struct addr_space *child_space = NULL;
	int err;

	if (!pid_out || !parent_task || !parent_task->process) {
		return -EINVAL;
	}
	if (!frame || !frame->user.rip || !frame->user.rsp) {
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
	child->user_entry = frame->user.rip;
	child->user_stack = frame->user.rsp;
	child->has_user_frame = true;
	child->user_frame = *frame;

	/*
	 * Splice the child into the parent's children list BEFORE making
	 * it runnable. sched_create_user_task() pushes the new task onto
	 * the run queue; if the LAPIC tick preempts us between that push
	 * and the splice, the child can run, exit (process_exit_current
	 * tries to wake parent->main_task — fine, parent is still
	 * RUNNING), and a concurrent process_wait on the parent would
	 * walk parent->first_child, fail to find the un-spliced ZOMBIE,
	 * and return -ECHILD while the child dangles forever. Doing the
	 * splice first under process_tree_lock closes the race.
	 */
	{
		uint64_t flags = spin_lock_irqsave(&process_tree_lock);

		child->next_sibling = parent->first_child;
		parent->first_child = child;
		spin_unlock_irqrestore(&process_tree_lock, flags);
	}

	err = sched_create_user_task("fork", child, NULL);
	if (err) {
		goto fail_unsplice;
	}

	*pid_out = child->pid;
	return 0;

fail_unsplice: {
	uint64_t flags = spin_lock_irqsave(&process_tree_lock);
	struct process **link = &parent->first_child;

	while (*link) {
		if (*link == child) {
			*link = child->next_sibling;
			child->next_sibling = NULL;
			break;
		}
		link = &(*link)->next_sibling;
	}
	spin_unlock_irqrestore(&process_tree_lock, flags);
}
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
