/*
 * kernel/kernel/retire.c — return-to-user pending-work handling.
 *
 * v0.0.4 (item 1: threads). arch_return_to_user_work() is invoked at
 * every boundary where the kernel is about to return to ring 3:
 *
 *   G1: syscall return  (kernel/arch/x86_64/syscall_entry.S)
 *   G2: IRQ return      (kernel/arch/x86_64/isr.S)
 *
 * It is the single place a thread acts on its TIF_NEED_DIE flag. By the
 * time we reach a gate the kernel call stack is fully unwound, so it is
 * safe to mark the task dead and schedule away (task_retire). Pollers
 * elsewhere (interruptible sleeps, long yield loops) only *return early*
 * toward a gate; they never retire directly. This is the same machinery
 * fatal-signal delivery will reuse.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/arch/arch_syscall.h>
#include <jnu/base/types.h>
#include <jnu/kernel/process.h>
#include <jnu/kernel/sched.h>

/* Retire the current thread. Never returns. */
static void task_retire(void)
{
	struct task *t = sched_current();
	struct process *proc = t ? t->process : NULL;
	int code = proc ? proc->group_exit_code : 0;

	process_thread_exit(code);
	__builtin_unreachable();
}

/*
 * Called from the assembly return-to-user gates with the kernel GS base
 * active. Returns normally if there is no pending work; does not return
 * if the thread must die.
 */
void arch_return_to_user_work(void)
{
	if (signal_pending()) {
		task_retire();
	}
}
