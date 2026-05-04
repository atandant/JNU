/*
 * include/jnu/sched.h - Round-robin scheduler core.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/context.h>
#include <jnu/cpu.h>
#include <jnu/types.h>

enum task_state {
	TASK_RUNNABLE,
	TASK_RUNNING,
	TASK_SLEEPING,
	TASK_ZOMBIE,
};

struct process;

struct task {
	int tid;
	int pid;
	enum task_state state;
	struct context ctx;
	void *kstack_base;
	void *kstack_top;
	struct process *process;
	struct task *parent;
	int exit_status;
	struct task *run_next;
	struct task *all_next;
	const char *name;
	/*
	 * Wake counter to close the missed-wakeup race between
	 * sched_wake() and sched_sleep_current(). When a waker calls
	 * sched_wake() while the target is still RUNNING (about to
	 * sleep), it bumps wake_pending. sched_sleep_current()
	 * consumes the credit instead of blocking.
	 */
	unsigned int wake_pending;
	/*
	 * v0.0.3 §2.9: per-task FS/GS base for arch_prctl.
	 * Saved and restored on every context switch via wrmsr.
	 * musl uses %fs:0 for TLS / errno access.
	 */
	uint64_t fs_base;
	uint64_t gs_base;
	/*
	 * v0.0.3 §2.7: per-task FPU/SSE state buffer.  64-byte
	 * aligned for FXSAVE/XSAVE requirements.  Sized to the
	 * maximum XSAVE area (1024 bytes covers x87+SSE; XSAVE
	 * with only XCR0.X87|SSE needs <= 576 bytes, but we
	 * over-provision to avoid a variable-length struct).
	 */
	_Alignas(64) uint8_t fpu_state[1024];
};

typedef void (*kernel_thread_fn)(void *arg);

void sched_init(void);
struct task *sched_current(void);
int sched_create_kernel_thread(const char *name, kernel_thread_fn fn, void *arg,
			       struct task **out);
int sched_create_user_task(const char *name, struct process *proc,
			   struct task **out);
void sched_yield(void);
void sched_exit_current(int status);
void sched_sleep_current(void);
void sched_wake(struct task *task);
void sched_tick(void);

/*
 * Free a TASK_ZOMBIE task. Removes it from `all_tasks`, releases its
 * kernel stack, and frees the `struct task` itself. The caller MUST
 * have already detached the task from any process linkage and confirmed
 * it is not the current task. Calling this on a non-zombie task is a
 * kernel bug. Used by process_wait's reaping path.
 */
void sched_reap_task(struct task *task);

int sched_selftest(void);
