/*
 * kernel/user/process.c - Process and PID scaffolding.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/base/types.h>
#include <jnu/kernel/panic.h>
#include <jnu/kernel/process.h>
#include <jnu/kernel/sched.h>
#include <jnu/lib/spinlock.h>
#include <jnu/mm/kmalloc.h>
#include <jnu/mm/vmm.h>
#include <jnu/user/fd.h>
#include <uapi/jnu/errno.h>

static int next_pid = 1;
/*
 * Visible to kernel/kernel/fork.c so process_fork() can splice the
 * child into the parent's child list under the same lock that
 * process_wait/process_exit_current use. Internal to process management
 * only — no other subsystem touches it.
 */
struct spinlock process_tree_lock = SPINLOCK_INITIALIZER;

/*
 * Init process pointer. Set once by main.c when the dedicated init
 * process is created. Used by process_exit_current
 * to reparent children of a dying process so they never leave a stale
 * `parent` pointer behind.
 */
static struct process *init_proc;

void process_set_init(struct process *proc) { init_proc = proc; }

struct process *process_get_init(void) { return init_proc; }

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
	/* v0.0.4: single-thread group at creation. */
	task->task_next = NULL;
	proc->tasks = task;
	proc->live_threads = 1;
	fd_table_init(&proc->fds);
	proc->space = vmm_kernel_space();
	task->process = proc;
	return proc;
}

void process_exit_current(int status)
{
	struct task *task = sched_current();
	struct process *proc = task ? task->process : NULL;
	struct process *init = init_proc;
	uint64_t flags;
	bool wake_init = false;

	if (!proc) {
		return;
	}

	for (int fd = 0; fd < JNU_MAX_FDS; fd++) {
		file_put(fd_close(&proc->fds, fd));
	}

	flags = spin_lock_irqsave(&process_tree_lock);

	/*
	 * Reparent every surviving child to init so they do not leave a
	 * dangling `parent` pointer behind once `proc` is reaped. If a
	 * child is already a zombie, it has been waiting for its parent
	 * to call wait4(); we splice it onto init's child list and wake
	 * init below so it can reap.
	 *
	 * If init itself is exiting we have nowhere to reparent onto;
	 * panic, because that is the same as PID 1 dying on Linux.
	 */
	if (proc == init) {
		if (proc->first_child) {
			panic("process_exit_current: init exited with live "
			      "children — system is unrecoverable");
		}
	} else if (proc->first_child) {
		struct process *head = proc->first_child;
		struct process *last = head;

		/*
		 * Walk the child list, repointing each child's parent
		 * pointer at init and noting any zombies we will need
		 * init to reap. Find the tail so we can splice the whole
		 * chain onto init->first_child in O(1).
		 */
		head->parent = init;
		if (head->state == PROCESS_ZOMBIE) {
			wake_init = true;
		}
		while (last->next_sibling) {
			last = last->next_sibling;
			last->parent = init;
			if (last->state == PROCESS_ZOMBIE) {
				wake_init = true;
			}
		}

		if (init) {
			last->next_sibling = init->first_child;
			init->first_child = head;
		}
		/*
		 * If init is NULL we are in pre-init boot and only
		 * kernel threads exist; they have no user children, so
		 * this branch is unreachable in practice. Either way,
		 * proc->first_child is cleared below to leave proc in a
		 * clean state for the upcoming reap.
		 */
	}
	proc->first_child = NULL;

	proc->exit_status = status & 0xFF;
	proc->state = PROCESS_ZOMBIE;
	if (proc->parent && proc->parent->main_task) {
		sched_wake(proc->parent->main_task);
	}
	spin_unlock_irqrestore(&process_tree_lock, flags);

	if (wake_init && init && init->main_task) {
		sched_wake(init->main_task);
	}
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
	/*
	 * v0.0.4: reap every task still attached to the group. Detached
	 * threads already self-reaped (TASK_DEAD); what remains here is
	 * the last/zombie thread that performed process teardown. Walk the
	 * list so no kernel stack is leaked for multi-threaded groups.
	 */
	{
		struct task *t = child->tasks;

		while (t) {
			struct task *next = t->task_next;

			sched_reap_task(t);
			t = next;
		}
		child->tasks = NULL;
		child->main_task = NULL;
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
		/*
		 * v0.0.4: death preempts a pending reap. If this thread is
		 * being torn down (exit_group / fatal), bail before
		 * touching the process tree so it can retire promptly.
		 */
		if (signal_pending()) {
			return -EINTR;
		}

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
		if (sched_sleep_interruptible() == -EINTR) {
			return -EINTR;
		}
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
