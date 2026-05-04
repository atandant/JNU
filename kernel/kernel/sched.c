/*
 * kernel/kernel/sched.c - Single-CPU round-robin scheduler.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 * note to self DO NOT rework this into a new scheduler
 * defer heavily since this is getting long and complex.
 * I'd prefer if we did it in another meaningful time.
 */

#include <jnu/arch_syscall.h>
#include <jnu/cpu.h>
#include <jnu/errno.h>
#include <jnu/gdt.h>
#include <jnu/klog.h>
#include <jnu/kmalloc.h>
#include <jnu/paging.h>
#include <jnu/pmm.h>
#include <jnu/process.h>
#include <jnu/sched.h>
#include <jnu/spinlock.h>
#include <jnu/string.h>
#include <jnu/types.h>
#include <jnu/usermode.h>
#include <jnu/vmm.h>

#define KSTACK_ORDER 2
#define KSTACK_SIZE PMM_ORDER_SIZE(KSTACK_ORDER)
#define SCHED_QUANTUM_TICKS 1

struct thread_boot {
	kernel_thread_fn fn;
	void *arg;
};

static struct spinlock sched_lock = SPINLOCK_INITIALIZER;
static struct task boot_task;
static struct task idle_task;
static struct task *current;
static struct task *runq_head;
static struct task *runq_tail;
static struct task *all_tasks;
static int next_tid = 1;
static unsigned quantum_left = SCHED_QUANTUM_TICKS;
static volatile uint64_t tick_count;
static volatile uint64_t preempt_pending;

static void idle_loop(void *arg);
static void kernel_thread_entry(struct thread_boot *boot);
static void user_thread_entry(void *arg);

static void runq_push(struct task *task)
{
	task->run_next = NULL;
	if (runq_tail) {
		runq_tail->run_next = task;
	} else {
		runq_head = task;
	}
	runq_tail = task;
}

static struct task *runq_pop(void)
{
	struct task *task = runq_head;

	if (!task) {
		return NULL;
	}
	runq_head = task->run_next;
	if (!runq_head) {
		runq_tail = NULL;
	}
	task->run_next = NULL;
	return task;
}

static void all_tasks_add(struct task *task)
{
	task->all_next = all_tasks;
	all_tasks = task;
}

static void task_prepare_stack(struct task *task, kernel_thread_fn fn,
			       void *arg)
{
	uintptr_t sp = (uintptr_t)task->kstack_top;
	struct thread_boot *boot;

	sp &= ~0xFull;
	sp -= sizeof(*boot);
	boot = (struct thread_boot *)sp;
	boot->fn = fn;
	boot->arg = arg;

	memset(&task->ctx, 0, sizeof(task->ctx));
	task->ctx.rsp = sp;
	task->ctx.rip = (uint64_t)(uintptr_t)kernel_thread_entry;
	task->ctx.rdi = (uint64_t)(uintptr_t)boot;
}

static void switch_to(struct task *next)
{
	struct task *prev = current;

	if (prev == next) {
		/*
		 * schedule_locked() may push us to the runq, find no
		 * other runnable task, and pop us right back. We left
		 * the state at TASK_RUNNABLE during the push; restore
		 * it to TASK_RUNNING so the next preempt cycle sees us
		 * as a real runnable task and pushes us again. Without
		 * this fix the task is silently dropped on the floor
		 * the moment any other task becomes runnable.
		 */
		next->state = TASK_RUNNING;
		quantum_left = SCHED_QUANTUM_TICKS;
		return;
	}

	current = next;
	next->state = TASK_RUNNING;
	quantum_left = SCHED_QUANTUM_TICKS;
	tss_set_rsp0((uint64_t)(uintptr_t)next->kstack_top);
	arch_syscall_set_kernel_stack((uint64_t)(uintptr_t)next->kstack_top);
	vmm_switch_to((next->process && next->process->space)
			  ? next->process->space
			  : vmm_kernel_space());

	/* v0.0.3 §2.7: eager FPU save/restore on every switch. */
	fpu_save(prev->fpu_state);
	fpu_restore(next->fpu_state);

	/* v0.0.3 §2.9: preserve per-task FS/GS base for TLS. */
	prev->fs_base = rdmsr(MSR_FS_BASE);
	wrmsr(MSR_FS_BASE, next->fs_base);

	context_switch(&prev->ctx, &next->ctx);
}

static void schedule_locked(void)
{
	struct task *prev = current;
	struct task *next;

	if (prev && prev->state == TASK_RUNNING && prev != &idle_task) {
		prev->state = TASK_RUNNABLE;
		runq_push(prev);
	}

	next = runq_pop();
	if (!next) {
		next = &idle_task;
	}

	switch_to(next);
}

static void kernel_thread_entry(struct thread_boot *boot)
{
	kernel_thread_fn fn = boot->fn;
	void *arg = boot->arg;

	spin_unlock_irqrestore(&sched_lock, 1ull << 9);

	fn(arg);
	sched_exit_current(0);
}

static void idle_loop(void *arg)
{
	(void)arg;
	for (;;) {
		__asm__ __volatile__("sti; hlt; cli");
		sched_yield();
	}
}

static void user_thread_entry(void *arg)
{
	struct process *proc = arg;

	/*
	 * kernel_thread_entry() already released sched_lock before
	 * calling us — do NOT unlock it again here.
	 */

	vmm_switch_to(proc->space);
	arch_syscall_set_kernel_stack(
	    (uint64_t)(uintptr_t)proc->main_task->kstack_top);
	if (proc->has_user_frame) {
		(void)usermode_enter_fork_frame(&proc->user_frame);
	} else {
		(void)usermode_enter(proc->user_entry, proc->user_stack);
	}
	sched_exit_current(127);
}

void sched_init(void)
{
	uint64_t rsp_now;

	memset(&boot_task, 0, sizeof(boot_task));
	boot_task.tid = next_tid++;
	boot_task.pid = process_alloc_pid();
	boot_task.state = TASK_RUNNING;
	boot_task.name = "boot";
	boot_task.kstack_top = NULL;
	__asm__ __volatile__("mov %%rsp, %0" : "=r"(rsp_now));
	boot_task.kstack_top = (void *)((rsp_now + 0xFFFull) & ~0xFFFull);
	boot_task.process = process_create_kernel(&boot_task);
	arch_syscall_set_kernel_stack(
	    (uint64_t)(uintptr_t)boot_task.kstack_top);
	current = &boot_task;
	fpu_state_init(boot_task.fpu_state);
	all_tasks_add(&boot_task);

	memset(&idle_task, 0, sizeof(idle_task));
	idle_task.tid = next_tid++;
	idle_task.pid = 0;
	idle_task.state = TASK_RUNNABLE;
	idle_task.name = "idle";
	idle_task.kstack_base =
	    phys_to_virt(pmm_alloc_zeroed_pages(KSTACK_ORDER));
	idle_task.kstack_top = (uint8_t *)idle_task.kstack_base + KSTACK_SIZE;
	task_prepare_stack(&idle_task, idle_loop, NULL);
	fpu_state_init(idle_task.fpu_state);
	all_tasks_add(&idle_task);

	pr_info("sched: boot tid=%d pid=%d, idle tid=%d\n", boot_task.tid,
		boot_task.pid, idle_task.tid);
}

int sched_create_user_task(const char *name, struct process *proc,
			   struct task **out)
{
	struct task *task;
	paddr_t stack_pa;
	uint64_t flags;
	int err;

	if (!proc || !proc->space || !proc->user_entry || !proc->user_stack) {
		return -EINVAL;
	}

	task = kzalloc(sizeof(*task));
	if (!task) {
		return -ENOMEM;
	}

	stack_pa = pmm_alloc_zeroed_pages(KSTACK_ORDER);
	if (!stack_pa) {
		err = -ENOMEM;
		goto fail_task;
	}

	task->tid = next_tid++;
	task->pid = proc->pid;
	task->state = TASK_RUNNABLE;
	task->kstack_base = phys_to_virt(stack_pa);
	task->kstack_top = (uint8_t *)task->kstack_base + KSTACK_SIZE;
	task->name = name ? name : "user";
	task->parent = current;
	task->process = proc;
	task_prepare_stack(task, user_thread_entry, proc);
	fpu_state_init(task->fpu_state);
	proc->main_task = task;

	flags = spin_lock_irqsave(&sched_lock);
	all_tasks_add(task);
	runq_push(task);
	spin_unlock_irqrestore(&sched_lock, flags);

	if (out) {
		*out = task;
	}
	return 0;

fail_task:
	kfree(task);
	return err;
}

struct task *sched_current(void) { return current; }

int sched_create_kernel_thread(const char *name, kernel_thread_fn fn, void *arg,
			       struct task **out)
{
	struct task *task;
	paddr_t stack_pa;
	uint64_t flags;
	int err;

	if (!fn) {
		return -EINVAL;
	}

	task = kzalloc(sizeof(*task));
	if (!task) {
		return -ENOMEM;
	}

	stack_pa = pmm_alloc_zeroed_pages(KSTACK_ORDER);
	if (!stack_pa) {
		err = -ENOMEM;
		goto fail_task;
	}

	task->tid = next_tid++;
	task->pid = process_alloc_pid();
	if (task->pid < 0) {
		err = task->pid;
		goto fail_stack;
	}
	task->state = TASK_RUNNABLE;
	task->kstack_base = phys_to_virt(stack_pa);
	task->kstack_top = (uint8_t *)task->kstack_base + KSTACK_SIZE;
	task->name = name ? name : "kthread";
	task->parent = current;
	task_prepare_stack(task, fn, arg);
	fpu_state_init(task->fpu_state);

	task->process = process_create_kernel(task);
	if (!task->process) {
		err = -ENOMEM;
		goto fail_pid;
	}

	flags = spin_lock_irqsave(&sched_lock);
	all_tasks_add(task);
	runq_push(task);
	spin_unlock_irqrestore(&sched_lock, flags);

	if (out) {
		*out = task;
	}
	return 0;

fail_pid:
	process_release_pid(task->pid);
fail_stack:
	pmm_free_pages(stack_pa, KSTACK_ORDER);
fail_task:
	kfree(task);
	return err;
}

void sched_yield(void)
{
	uint64_t flags = spin_lock_irqsave(&sched_lock);
	schedule_locked();
	spin_unlock_irqrestore(&sched_lock, flags);
}

void sched_exit_current(int status)
{
	uint64_t flags = spin_lock_irqsave(&sched_lock);
	current->exit_status = status & 0xFF;
	current->state = TASK_ZOMBIE;
	schedule_locked();
	spin_unlock_irqrestore(&sched_lock, flags);

	for (;;) {
		__asm__ __volatile__("cli; hlt");
	}
}

void sched_sleep_current(void)
{
	uint64_t flags = spin_lock_irqsave(&sched_lock);

	/*
	 * Consume any wake credit posted before we got the lock. This
	 * is the missed-wakeup guard: a waker that ran between the
	 * caller's predicate check and this point bumped wake_pending
	 * but found us still RUNNING, so it skipped the runq push.
	 * Without consuming the credit here we would block forever.
	 */
	if (current->wake_pending > 0) {
		current->wake_pending--;
		spin_unlock_irqrestore(&sched_lock, flags);
		return;
	}

	current->state = TASK_SLEEPING;
	schedule_locked();
	spin_unlock_irqrestore(&sched_lock, flags);
}

void sched_wake(struct task *task)
{
	uint64_t flags;

	if (!task) {
		return;
	}

	flags = spin_lock_irqsave(&sched_lock);
	/*
	 * Always post a wake credit. If the target is already sleeping
	 * we additionally make it runnable; if it is racing toward
	 * sched_sleep_current() the credit will be consumed there.
	 */
	task->wake_pending++;
	if (task->state == TASK_SLEEPING) {
		task->wake_pending--;
		task->state = TASK_RUNNABLE;
		runq_push(task);
	}
	spin_unlock_irqrestore(&sched_lock, flags);
}

void sched_tick(void)
{
	uint64_t flags;

	tick_count++;
	if (quantum_left > 0) {
		quantum_left--;
	}
	if (quantum_left != 0) {
		return;
	}

	/*
	 * Quantum expired. Preempt the current task. We are in IRQ
	 * context with interrupts disabled, so sched_lock cannot be
	 * held by anyone else on this single-CPU build (every holder
	 * disables interrupts before acquiring it). The IRQ frame on
	 * the current kernel stack becomes part of the saved state of
	 * the preempted task; when this task is scheduled back in,
	 * context_switch returns here and we ride the same IRQ frame
	 * back out through isr.S.
	 */
	preempt_pending++;
	quantum_left = SCHED_QUANTUM_TICKS;
	flags = spin_lock_irqsave(&sched_lock);
	schedule_locked();
	spin_unlock_irqrestore(&sched_lock, flags);
}

static void selftest_thread(void *arg)
{
	volatile int *counter = arg;

	for (int i = 0; i < 8; i++) {
		(*counter)++;
		sched_yield();
	}
}

static void all_tasks_remove(struct task *task)
{
	struct task **link = &all_tasks;

	while (*link) {
		if (*link == task) {
			*link = task->all_next;
			task->all_next = NULL;
			return;
		}
		link = &(*link)->all_next;
	}
}

void sched_reap_task(struct task *task)
{
	uint64_t flags;

	if (!task) {
		return;
	}
	if (task == current) {
		/*
		 * Reaping the running task is a kernel bug: we would
		 * release the very kstack we are executing on. Bail
		 * loudly rather than silently corrupting the world.
		 */
		pr_err("sched_reap_task: refusing to reap the running task "
		       "tid=%d\n",
		       task->tid);
		return;
	}

	flags = spin_lock_irqsave(&sched_lock);
	all_tasks_remove(task);
	spin_unlock_irqrestore(&sched_lock, flags);

	if (task->kstack_base) {
		pmm_free_pages(virt_to_phys(task->kstack_base), KSTACK_ORDER);
	}
	kfree(task);
}

int sched_selftest(void)
{
	volatile int a = 0;
	volatile int b = 0;
	struct pmm_stats before;
	struct pmm_stats after;
	int err;

	pmm_get_stats(&before);
	err = sched_create_kernel_thread("selftest-a", selftest_thread,
					 (void *)&a, NULL);
	if (err) {
		return err;
	}
	err = sched_create_kernel_thread("selftest-b", selftest_thread,
					 (void *)&b, NULL);
	if (err) {
		return err;
	}

	while (a < 8 || b < 8) {
		sched_yield();
	}

	pmm_get_stats(&after);
	if (after.free_pages > before.free_pages) {
		return -EINVAL;
	}

	pr_info("sched: selftest ticks=%lu pending-preempt=%lu\n",
		(unsigned long)tick_count, (unsigned long)preempt_pending);
	return 0;
}
