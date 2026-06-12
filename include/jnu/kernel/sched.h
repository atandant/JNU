/*
 * include/jnu/kernel/sched.h - Round-robin scheduler core.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/arch/context.h>
#include <jnu/arch/cpu.h>
#include <jnu/base/types.h>
#include <jnu/user/syscall.h>

enum task_state {
	TASK_RUNNABLE,
	TASK_RUNNING,
	TASK_SLEEPING,
	TASK_ZOMBIE,
	/*
	 * v0.0.4: a detached thread (non-last in its thread group) that
	 * has exited. Unlike TASK_ZOMBIE — which waits for a parent
	 * wait4() to reap it — a TASK_DEAD task is self-reaped: the
	 * scheduler frees its kernel stack and struct task on the next
	 * context switch (see sched_finish_switch / reap_zombie). The
	 * task is never observed by wait4().
	 */
	TASK_DEAD,
};

/*
 * Per-task pending-work flags. Checked at every return-to-userspace
 * boundary (syscall return G1, IRQ return G2) and after interruptible
 * sleeps (P1) / inside long kernel yield loops (P2). Atomic so the SMP
 * future needs no API change.
 */
#define TIF_NEED_DIE (1u << 0)

/*
 * v0.0.4: clone() DoS guards. Per-group cap bounds kernel stacks in one
 * address space; global cap bounds struct task + kstack allocations
 * system-wide (matches Linux returning EAGAIN when limits are hit).
 */
#define JNU_MAX_THREADS_PER_GROUP 256
#define JNU_MAX_TASKS 512

struct process;
struct syscall_frame;

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
	/*
	 * v0.0.4: intra-thread-group list. All tasks belonging to the
	 * same struct process (thread group) are linked here via
	 * proc->tasks. Distinct from run_next (run queue) and all_next
	 * (global task list).
	 */
	struct task *task_next;
	const char *name;
	/*
	 * v0.0.4: pending-work flags (TIF_*). Set by the group-exit /
	 * future signal path; consumed at return-to-user boundaries.
	 */
	uint32_t flags;
	/*
	 * v0.0.4: CLONE_CHILD_CLEARTID / set_tid_address pointer. On
	 * thread exit the kernel writes 0 here and (TODO: item 2) issues
	 * a FUTEX_WAKE so pthread_join() can complete.
	 */
	void *clear_child_tid;
	/*
	 * v0.0.4: CLONE_CHILD_SETTID pointer. Written with the new tid by
	 * the *child itself* at first userspace entry (thread_user_entry),
	 * not by the parent, so the child cannot observe a stale slot
	 * before the store lands.
	 */
	void *set_child_tid;
	/*
	 * v0.0.4: per-thread first-entry state. A cloned thread resumes
	 * in userspace from this frame (parent's syscall frame with rsp
	 * overridden to the clone child_stack and rax forced to 0).
	 */
	bool has_user_frame;
	struct syscall_frame user_frame;
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
int sched_create_thread_task(struct process *proc,
			     const struct syscall_frame *parent_frame,
			     uint64_t child_stack, uint64_t tls,
			     void *clear_child_tid, void *set_child_tid,
			     struct task **out);
void sched_yield(void);
void sched_exit_current(int status);
/*
 * v0.0.4: exit a detached thread (non-last in its group). Marks the
 * caller TASK_DEAD and schedules away forever; the next task frees this
 * task's kernel stack and struct task. Never returns.
 */
void sched_exit_detached(void);
void sched_sleep_current(void);
/*
 * v0.0.4: interruptible sleep. Returns 0 if woken normally, -EINTR if a
 * pending-death flag (TIF_NEED_DIE) is set on the caller. Used by
 * blocking syscalls (e.g. wait4) so a group-exit can unwind them.
 */
int sched_sleep_interruptible(void);
void sched_wake(struct task *task);
void sched_tick(void);

/* v0.0.4: true if the current task has a pending death/work flag. */
bool signal_pending(void);

/*
 * Free a TASK_ZOMBIE task. Removes it from `all_tasks`, releases its
 * kernel stack, and frees the `struct task` itself. The caller MUST
 * have already detached the task from any process linkage and confirmed
 * it is not the current task. Calling this on a non-zombie task is a
 * kernel bug. Used by process_wait's reaping path.
 */
void sched_reap_task(struct task *task);

int sched_selftest(void);
