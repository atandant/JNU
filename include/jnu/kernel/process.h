/*
 * include/jnu/kernel/process.h - Process object scaffolding.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/base/types.h>
#include <jnu/mm/vmm.h>
#include <jnu/user/fd.h>
#include <jnu/user/syscall.h>

struct task;

enum process_state {
	PROCESS_ALIVE,
	PROCESS_ZOMBIE,
};

struct process {
	int pid;
	enum process_state state;
	/*
	 * main_task is the thread-group "leader" — the task wait4() wakes
	 * and the fallback target for parent notification. v0.0.4: a
	 * process may now own multiple tasks (threads); they are linked
	 * via `tasks` and counted by `live_threads`. When all threads
	 * exit (live_threads hits 0) the process becomes a zombie.
	 */
	struct task *main_task;
	struct task *tasks; /* thread-group task list (task_next) */
	int live_threads;   /* number of non-exited tasks in group */
	struct process *parent;
	struct process *first_child;
	struct process *next_sibling;
	int exit_status;
	/*
	 * v0.0.4: exit_group() status, recorded so whichever thread is
	 * last out reports the group's code regardless of teardown order.
	 */
	int group_exit_code;
	struct fd_table fds;
	struct addr_space *space;
	uint64_t user_entry;
	uint64_t user_stack;
	bool has_user_frame;
	struct syscall_frame user_frame;
};

int process_alloc_pid(void);
void process_release_pid(int pid);

struct process *process_create_kernel(struct task *task);
void process_exit_current(int status);

/*
 * v0.0.4: exit a single thread of the current thread group. Decrements
 * live_threads. If other threads remain the caller self-reaps as a
 * detached TASK_DEAD task; if it is the last thread it performs full
 * process teardown (process_exit_current) and becomes a TASK_ZOMBIE for
 * the parent to wait4(). Never returns. This is the back end of both
 * sys_exit() and a TIF_NEED_DIE retirement.
 */
void process_thread_exit(int status);

/*
 * v0.0.4: terminate the entire current thread group (exit_group). Sets
 * TIF_NEED_DIE on every sibling task and wakes any that are sleeping so
 * they unwind and retire at their next return-to-user boundary, then
 * exits the calling thread. Never returns.
 */
void process_group_exit(int status);

/*
 * v0.0.4: add a new thread (task) to the current process. Implements
 * the clone(CLONE_VM|CLONE_THREAD|...) path: shares the caller's
 * address space and fd table, allocates a fresh tid (same pid/tgid),
 * and schedules the new task to resume in userspace on `child_stack`
 * with rax = 0 and FS base = `tls`. `*tid_out` receives the new tid.
 */
int process_clone_thread(const struct syscall_frame *frame,
			 uint64_t child_stack, uint64_t tls, void *parent_tid,
			 void *child_tid, uint64_t flags, int *tid_out);

int process_wait(int pid, int *status_out);
void process_destroy(struct process *proc);

/*
 * Register `proc` as the init process. Called once from main.c after
 * start_init() has loaded /init into a dedicated user process. Any
 * process that subsequently exits while it still has live (non-zombie)
 * children reparents those children to `proc` so they are not orphaned
 * onto a freed `parent` pointer. Init must outlive every other process;
 * if init itself ever exits, the kernel panics — there is nothing to
 * reparent its children onto.
 */
void process_set_init(struct process *proc);
struct process *process_get_init(void);

/*
 * Duplicate the calling process. Allocates a child process, deep-copies
 * the parent's address space and fd table, then schedules a user task
 * that resumes in userspace at `user_rip`/`user_rsp` with rax = 0.
 *
 * `*pid_out` receives the child pid in the parent on success. The
 * child does not "see" this pid because `process_fork` is the parent's
 * call: the child wakes up directly in userspace with the forged
 * register file. Returns 0 / -errno.
 */
int process_fork(const struct syscall_frame *frame, int *pid_out);

int process_selftest(void);
