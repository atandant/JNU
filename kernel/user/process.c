/*
 * kernel/user/process.c - Process and PID scaffolding.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/fd.h>
#include <jnu/kmalloc.h>
#include <jnu/process.h>
#include <jnu/sched.h>
#include <jnu/spinlock.h>
#include <jnu/types.h>
#include <jnu/vmm.h>

static int next_pid = 1;
/*
 * Visible to kernel/kernel/fork.c so process_fork() can splice the
 * child into the parent's child list under the same lock that
 * process_wait/process_exit_current use. Internal to process management
 * only — no other subsystem touches it.
 */
struct spinlock process_tree_lock = SPINLOCK_INITIALIZER;

int process_alloc_pid(void)
{
	int pid = __atomic_fetch_add(&next_pid, 1, __ATOMIC_RELAXED);
	if (pid <= 0) {
		return -ENOMEM;
	}
	return pid;
}

void process_release_pid(int pid) { (void)pid; }

struct process *process_create_kernel(struct task *task)
{
	struct process *proc = kzalloc(sizeof(*proc));

	if (!proc) {
		return NULL;
	}

	proc->pid = task->pid;
	proc->state = PROCESS_ALIVE;
	proc->main_task = task;
	fd_table_init(&proc->fds);
	proc->space = vmm_kernel_space();
	task->process = proc;
	return proc;
}

void process_exit_current(int status)
{
	struct task *task = sched_current();
	struct process *proc = task ? task->process : NULL;
	uint64_t flags;

	if (!proc) {
		return;
	}

	for (int fd = 0; fd < JNU_MAX_FDS; fd++) {
		file_put(fd_close(&proc->fds, fd));
	}

	flags = spin_lock_irqsave(&process_tree_lock);
	proc->exit_status = status & 0xFF;
	proc->state = PROCESS_ZOMBIE;
	if (proc->parent && proc->parent->main_task) {
		sched_wake(proc->parent->main_task);
	}
	spin_unlock_irqrestore(&process_tree_lock, flags);
}

static struct process *find_child(struct process *parent, int pid)
{
	for (struct process *child = parent->first_child; child;
	     child = child->next_sibling) {
		if (pid == -1 || child->pid == pid) {
			return child;
		}
	}
	return NULL;
}

static void unlink_child(struct process *parent, struct process *target)
{
	struct process **link = &parent->first_child;

	while (*link) {
		if (*link == target) {
			*link = target->next_sibling;
			target->next_sibling = NULL;
			target->parent = NULL;
			return;
		}
		link = &(*link)->next_sibling;
	}
}

/*
 * Release every resource the child held: the address space (page
 * tables + user pages), the kernel task (kstack + struct task), and
 * the process struct itself. Called by process_wait after unlinking
 * the zombie. The child's fd table was already drained by
 * process_exit_current; the loop here is a paranoia sweep that no-ops
 * on an already-empty table.
 */
static void process_reap(struct process *child)
{
	if (!child) {
		return;
	}

	for (int fd = 0; fd < JNU_MAX_FDS; fd++) {
		file_put(fd_close(&child->fds, fd));
	}
	if (child->space && child->space != vmm_kernel_space()) {
		vmm_destroy_space(child->space);
	}
	if (child->main_task) {
		sched_reap_task(child->main_task);
	}
	process_release_pid(child->pid);
	kfree(child);
}

int process_wait(int pid, int *status_out)
{
	struct task *task = sched_current();
	struct process *parent = task ? task->process : NULL;
	struct process *child;
	uint64_t flags;

	if (!parent || (pid != -1 && pid <= 0)) {
		return -EINVAL;
	}

	for (;;) {
		flags = spin_lock_irqsave(&process_tree_lock);
		child = find_child(parent, pid);
		if (!child) {
			spin_unlock_irqrestore(&process_tree_lock, flags);
			return -ECHILD;
		}
		if (child->state == PROCESS_ZOMBIE) {
			int child_pid = child->pid;

			if (status_out) {
				*status_out = child->exit_status;
			}
			unlink_child(parent, child);
			spin_unlock_irqrestore(&process_tree_lock, flags);

			/*
			 * Reap outside the tree lock: vmm_destroy_space
			 * walks page tables and frees pages, which may
			 * be slow and must not block IRQ delivery for
			 * an unbounded time.
			 */
			process_reap(child);
			return child_pid;
		}
		spin_unlock_irqrestore(&process_tree_lock, flags);
		sched_sleep_current();
	}
}

void process_destroy(struct process *proc) { kfree(proc); }

int process_selftest(void)
{
	int pid = process_alloc_pid();

	if (pid <= 0) {
		return -EINVAL;
	}
	process_release_pid(pid);
	return 0;
}
