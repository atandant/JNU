/*
 * kernel/kernel/clone.c — thread (clone) creation and thread-group exit.
 *
 * v0.0.4 (item 1: threads). Implements the thread half of the process
 * model: process_clone_thread() adds a task to the current thread group
 * (CLONE_VM | CLONE_THREAD), and process_thread_exit() /
 * process_group_exit() implement the cooperative-unwind teardown model
 * debated in the threads roadmap:
 *
 *   - A non-last thread exiting self-reaps as a detached TASK_DEAD task
 *     (its kernel stack is freed by the scheduler on the next switch).
 *   - The last thread to exit performs full process teardown via
 *     process_exit_current() and becomes a TASK_ZOMBIE for wait4().
 *   - process_group_exit() flags every sibling with TIF_NEED_DIE and
 *     wakes sleepers so they unwind at their next return-to-user gate.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/base/types.h>
#include <jnu/kernel/process.h>
#include <jnu/kernel/sched.h>
#include <jnu/lib/klog.h>
#include <jnu/lib/spinlock.h>
#include <jnu/user/syscall.h>
#include <jnu/user/usercopy.h>
#include <uapi/jnu/errno.h>
#include <uapi/jnu/sched.h>

extern struct spinlock process_tree_lock;

/*
 * Accepted clone() flag set: exactly what musl's pthread_create()
 * passes. CLONE_VM and CLONE_THREAD are mandatory (we only support
 * same-address-space threads, not vfork-style shared-VM processes).
 */
#define CLONE_THREAD_FLAGS                                                     \
	(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |    \
	 CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID |                  \
	 CLONE_CHILD_CLEARTID | CLONE_DETACHED | CLONE_CHILD_SETTID)

/*
 * Write 0 to the exiting thread's clear_child_tid and (TODO item 2)
 * issue a FUTEX_WAKE so a pthread_join() blocked on that word can make
 * progress. Until futex lands, join spin-waits on the cleared word.
 */
static void clear_child_tid(struct task *t)
{
	int zero = 0;

	if (!t || !t->clear_child_tid) {
		return;
	}
	(void)copy_to_user(t->clear_child_tid, &zero, sizeof(zero));
	t->clear_child_tid = NULL;
	/* TODO(item 2: futex): futex_wake((uint32_t *)addr, 1); */
}

/* Remove `task` from its thread group's task list. Caller holds lock. */
static void group_remove_task(struct process *proc, struct task *task)
{
	struct task **link = &proc->tasks;

	while (*link) {
		if (*link == task) {
			*link = task->task_next;
			task->task_next = NULL;
			return;
		}
		link = &(*link)->task_next;
	}
}

void process_thread_exit(int status)
{
	struct task *self = sched_current();
	struct process *proc = self ? self->process : NULL;
	uint64_t flags;
	int remaining;

	if (!proc) {
		sched_exit_current(status);
		__builtin_unreachable();
	}

	flags = spin_lock_irqsave(&process_tree_lock);
	remaining = --proc->live_threads;

	if (remaining > 0) {
		/* Detached: drop ourselves from the group and self-reap. */
		group_remove_task(proc, self);
		if (proc->main_task == self) {
			proc->main_task = proc->tasks;
		}
		spin_unlock_irqrestore(&process_tree_lock, flags);

		clear_child_tid(self);
		sched_exit_detached();
		__builtin_unreachable();
	}

	/*
	 * Last thread out. We stay linked in proc->tasks so process_reap()
	 * can free our kernel stack after the parent wait4()s us.
	 */
	spin_unlock_irqrestore(&process_tree_lock, flags);

	clear_child_tid(self);
	process_exit_current(status); /* reparent, zombie, wake parent */
	sched_exit_current(status);   /* TASK_ZOMBIE, schedule away */
	__builtin_unreachable();
}

void process_group_exit(int status)
{
	struct task *self = sched_current();
	struct process *proc = self ? self->process : NULL;
	uint64_t flags;

	if (!proc) {
		sched_exit_current(status);
		__builtin_unreachable();
	}

	flags = spin_lock_irqsave(&process_tree_lock);

	/* Record the group exit code so whichever thread is last reports
	 * it, regardless of which one wins the race to teardown. */
	proc->group_exit_code = status;

	for (struct task *t = proc->tasks; t; t = t->task_next) {
		if (t == self) {
			continue;
		}
		__atomic_or_fetch(&t->flags, TIF_NEED_DIE, __ATOMIC_RELEASE);
		/* Wake sleepers so they unwind to their return-to-user
		 * gate and retire; runnable siblings are caught when they
		 * next surface. Lock order (tree -> sched) matches
		 * process_exit_current's parent wake. */
		if (t->state == TASK_SLEEPING) {
			sched_wake(t);
		}
	}

	spin_unlock_irqrestore(&process_tree_lock, flags);

	/* Now exit ourselves through the normal thread-exit path. */
	process_thread_exit(status);
	__builtin_unreachable();
}

int process_clone_thread(const struct syscall_frame *frame, uint64_t child_stack,
			 uint64_t tls, void *parent_tid, void *child_tid,
			 uint64_t flags, int *tid_out)
{
	struct task *self = sched_current();
	struct process *proc = self ? self->process : NULL;
	struct task *task = NULL;
	void *cct;
	void *sct;
	uint64_t tree_flags;
	int err;

	if (!proc || !tid_out || !frame) {
		return -EINVAL;
	}

	/* We only support same-process thread creation. */
	if (!(flags & CLONE_VM) || !(flags & CLONE_THREAD)) {
		return -ENOSYS;
	}
	if (!child_stack) {
		return -EINVAL;
	}
	if (!user_range_ok((void *)(uintptr_t)child_stack, 1)) {
		return -EFAULT;
	}
	/* Reject any flag we do not understand to avoid silent ABI drift. */
	if (flags & ~(uint64_t)CLONE_THREAD_FLAGS) {
		return -ENOSYS;
	}

	cct = (flags & CLONE_CHILD_CLEARTID) ? child_tid : NULL;
	sct = (flags & CLONE_CHILD_SETTID) ? child_tid : NULL;

	tree_flags = spin_lock_irqsave(&process_tree_lock);
	if (proc->live_threads >= JNU_MAX_THREADS_PER_GROUP) {
		spin_unlock_irqrestore(&process_tree_lock, tree_flags);
		return -EAGAIN;
	}
	proc->live_threads++;
	spin_unlock_irqrestore(&process_tree_lock, tree_flags);

	err = sched_create_thread_task(proc, frame, child_stack,
				       (flags & CLONE_SETTLS) ? tls : self->fs_base,
				       cct, sct, &task);
	if (err) {
		tree_flags = spin_lock_irqsave(&process_tree_lock);
		proc->live_threads--;
		spin_unlock_irqrestore(&process_tree_lock, tree_flags);
		return err;
	}

	if (flags & CLONE_PARENT_SETTID) {
		(void)copy_to_user(parent_tid, &task->tid, sizeof(task->tid));
	}

	*tid_out = task->tid;
	return 0;
}
