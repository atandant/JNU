/*
 * include/jnu/sched.h - Round-robin scheduler core.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/context.h>
#include <jnu/types.h>

enum task_state {
	TASK_RUNNABLE,
	TASK_RUNNING,
	TASK_SLEEPING,
	TASK_ZOMBIE,
};

struct process;

struct task {
	int			tid;
	int			pid;
	enum task_state		state;
	struct context		ctx;
	void			*kstack_base;
	void			*kstack_top;
	struct process		*process;
	struct task		*parent;
	int			exit_status;
	struct task		*run_next;
	struct task		*all_next;
	const char		*name;
	/*
	 * Wake counter to close the missed-wakeup race between
	 * sched_wake() and sched_sleep_current(). When a waker calls
	 * sched_wake() while the target is still RUNNING (about to
	 * sleep), it bumps wake_pending. sched_sleep_current()
	 * consumes the credit instead of blocking.
	 */
	unsigned int		wake_pending;
};

typedef void (*kernel_thread_fn)(void *arg);

void sched_init(void);
struct task *sched_current(void);
int sched_create_kernel_thread(const char *name, kernel_thread_fn fn,
			       void *arg, struct task **out);
int sched_create_user_task(const char *name, struct process *proc,
			   struct task **out);
void sched_yield(void);
void sched_exit_current(int status);
void sched_sleep_current(void);
void sched_wake(struct task *task);
void sched_tick(void);
int sched_selftest(void);
