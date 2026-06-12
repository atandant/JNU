/*
 * kernel/kernel/sched.c - Single-CPU round-robin scheduler.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 * note to self DO NOT rework this into a new scheduler
 * defer heavily since this is getting long and complex.
 * I'd prefer if we did it in another meaningful time.
 */

#include <jnu/arch/arch_syscall.h>
#include <jnu/arch/cpu.h>
#include <jnu/arch/gdt.h>
#include <jnu/arch/usermode.h>
#include <jnu/base/types.h>
#include <jnu/kernel/process.h>
#include <jnu/kernel/sched.h>
#include <jnu/lib/klog.h>
#include <jnu/lib/spinlock.h>
#include <jnu/lib/string.h>
#include <jnu/mm/kmalloc.h>
#include <jnu/mm/paging.h>
#include <jnu/mm/pmm.h>
#include <jnu/mm/vmm.h>
#include <jnu/user/usercopy.h>
#include <uapi/jnu/errno.h>

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
static int nr_tasks;
static unsigned quantum_left = SCHED_QUANTUM_TICKS;
static volatile uint64_t tick_count;
static volatile uint64_t preempt_pending;

/*
 * v0.0.4: deferred reap slot for detached threads. A TASK_DEAD task
 * cannot free the kernel stack it is executing on, so switch_to()
 * records it here just before context-switching away; the next task to
 * run frees it in sched_finish_switch() while still holding sched_lock.
 */
static struct task *reap_zombie;

static void idle_loop(void *arg);
static void kernel_thread_entry(struct thread_boot *boot);
static void user_thread_entry(void *arg);
static void thread_user_entry(void *arg);
static void sched_finish_switch(void);

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
	nr_tasks++;
}

static void all_tasks_remove(struct task *task);

/*
 * v0.0.4: free the previous task if it exited detached (TASK_DEAD).
 * Runs in the context of the freshly-scheduled task with sched_lock
 * held. Frees the dead task's kernel stack and struct task. The actual
 * frees touch the PMM/heap, which never call back into the scheduler,
 * so there is no lock inversion against sched_lock.
 */
static void sched_finish_switch(void)
{
	struct task *z = reap_zombie;

	if (!z) {
		return;
	}
	reap_zombie = NULL;

	all_tasks_remove(z);
	if (z->kstack_base) {
		pmm_free_pages(virt_to_phys(z->kstack_base), KSTACK_ORDER);
	}
	kfree(z);
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

	/*
	 * v0.0.4: if we are switching away from a detached thread that
	 * just exited, hand it to the next task for reaping. We cannot
	 * free our own kernel stack here.
	 */
	if (prev->state == TASK_DEAD) {
		reap_zombie = prev;
	}

	context_switch(&prev->ctx, &next->ctx);

	/* We have just been scheduled back in: reap any predecessor. */
	sched_finish_switch();
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

	/*
	 * v0.0.4: a freshly created task starts here instead of returning
	 * through switch_to(), so reap any detached predecessor before we
	 * drop sched_lock.
	 */
	sched_finish_switch();
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

/*
 * v0.0.4: first-run trampoline for a cloned thread. Unlike
 * user_thread_entry (which reads the per-process frame), this resumes
 * from the per-task frame forged by sched_create_thread_task: the
 * parent's syscall frame with rsp = child_stack and rax forced to 0.
 */
static void thread_user_entry(void *arg)
{
	struct task *t = arg;
	struct process *proc = t->process;

	vmm_switch_to(proc->space);
	arch_syscall_set_kernel_stack((uint64_t)(uintptr_t)t->kstack_top);
	/*
	 * CLONE_CHILD_SETTID: publish tid from the child's context before
	 * any userspace instruction runs, so the child cannot observe a
	 * stale slot (Linux does the same store here, not in the parent).
	 */
	if (t->set_child_tid) {
		(void)copy_to_user(t->set_child_tid, &t->tid, sizeof(t->tid));
		t->set_child_tid = NULL;
	}
	(void)usermode_enter_fork_frame(&t->user_frame);
	sched_exit_current(127);
}

void sched_init(void)
{
	uint64_t rsp_now;

	memset(&boot_task, 0, sizeof(boot_task));
	boot_task.pid = process_alloc_pid();
	/* Thread-group leader: tid == pid, drawn from the single id pool. */
	boot_task.tid = boot_task.pid;
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
	idle_task.tid = process_alloc_pid();
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

	/* Thread-group leader: tid == tgid (== proc->pid). */
	task->tid = proc->pid;
	task->pid = proc->pid;
	task->state = TASK_RUNNABLE;
	task->kstack_base = phys_to_virt(stack_pa);
	task->kstack_top = (uint8_t *)task->kstack_base + KSTACK_SIZE;
	task->name = name ? name : "user";
	task->parent = current;
	if (current) {
		task->fs_base = current->fs_base;
		task->gs_base = current->gs_base;
	}
	task->process = proc;
	task_prepare_stack(task, user_thread_entry, proc);
	fpu_state_init(task->fpu_state);
	proc->main_task = task;
	/* v0.0.4: this is the thread-group leader and only thread. */
	task->task_next = NULL;
	proc->tasks = task;
	proc->live_threads = 1;

	flags = spin_lock_irqsave(&sched_lock);
	if (nr_tasks >= JNU_MAX_TASKS) {
		spin_unlock_irqrestore(&sched_lock, flags);
		err = -EAGAIN;
		goto fail_stack;
	}
	all_tasks_add(task);
	runq_push(task);
	spin_unlock_irqrestore(&sched_lock, flags);

	if (out) {
		*out = task;
	}
	return 0;

fail_stack:
	pmm_free_pages(stack_pa, KSTACK_ORDER);
fail_task:
	kfree(task);
	return err;
}

int sched_create_thread_task(struct process *proc,
			     const struct syscall_frame *parent_frame,
			     uint64_t child_stack, uint64_t tls,
			     void *clear_child_tid, void *set_child_tid,
			     struct task **out)
{
	struct task *task;
	paddr_t stack_pa;
	uint64_t flags;
	int err;

	if (!proc || !proc->space || !parent_frame || !child_stack) {
		return -EINVAL;
	}
	if (!user_range_ok((void *)(uintptr_t)child_stack, 1)) {
		return -EFAULT;
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

	task->tid = process_alloc_pid();
	if (task->tid < 0) {
		err = task->tid;
		goto fail_stack;
	}
	task->pid = proc->pid; /* same tgid as the rest of the group */
	task->state = TASK_RUNNABLE;
	task->kstack_base = phys_to_virt(stack_pa);
	task->kstack_top = (uint8_t *)task->kstack_base + KSTACK_SIZE;
	task->name = "thread";
	task->parent = current;
	task->process = proc;
	task->fs_base = tls; /* CLONE_SETTLS */
	task->gs_base = current ? current->gs_base : 0;
	task->clear_child_tid = clear_child_tid; /* CLONE_CHILD_CLEARTID */
	task->set_child_tid = set_child_tid;     /* CLONE_CHILD_SETTID */

	/*
	 * Forge the child's first userspace return: clone of the parent's
	 * frame with the stack pointer redirected to child_stack. rax is
	 * forced to 0 by usermode_enter_fork_frame, satisfying the clone
	 * ABI's child-return contract.
	 */
	task->user_frame = *parent_frame;
	task->user_frame.user.rsp = child_stack;
	task->has_user_frame = true;
	task_prepare_stack(task, thread_user_entry, task);
	fpu_state_init(task->fpu_state);

	flags = spin_lock_irqsave(&sched_lock);
	if (nr_tasks >= JNU_MAX_TASKS) {
		spin_unlock_irqrestore(&sched_lock, flags);
		err = -EAGAIN;
		goto fail_tid;
	}
	/* Link into the thread group and the global/run lists. */
	task->task_next = proc->tasks;
	proc->tasks = task;
	all_tasks_add(task);
	runq_push(task);
	spin_unlock_irqrestore(&sched_lock, flags);

	if (out) {
		*out = task;
	}
	return 0;

fail_tid:
	process_release_pid(task->tid);
fail_stack:
	pmm_free_pages(stack_pa, KSTACK_ORDER);
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

	task->pid = process_alloc_pid();
	if (task->pid < 0) {
		err = task->pid;
		goto fail_stack;
	}
	/* Thread-group leader: tid == pid. */
	task->tid = task->pid;
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
	if (nr_tasks >= JNU_MAX_TASKS) {
		spin_unlock_irqrestore(&sched_lock, flags);
		err = -EAGAIN;
		goto fail_pid;
	}
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

void sched_exit_detached(void)
{
	uint64_t flags = spin_lock_irqsave(&sched_lock);

	/*
	 * Mark ourselves dead and schedule away. switch_to() records us
	 * in reap_zombie; the next task frees our kernel stack and struct
	 * task in sched_finish_switch(). We never run again.
	 */
	current->state = TASK_DEAD;
	schedule_locked();
	spin_unlock_irqrestore(&sched_lock, flags);

	for (;;) {
		__asm__ __volatile__("cli; hlt");
	}
}

bool signal_pending(void)
{
	struct task *t = current;

	return t && (__atomic_load_n(&t->flags, __ATOMIC_ACQUIRE) &
		     TIF_NEED_DIE) != 0;
}

int sched_sleep_interruptible(void)
{
	return sched_sleep_timed_interruptible(0);
}

int sched_sleep_timed_interruptible(uint64_t timeout_us)
{
	uint64_t flags;
	uint64_t deadline = 0;

	if (signal_pending()) {
		return -EINTR;
	}

	if (timeout_us != 0) {
		uint64_t now = cpu_us_since_boot();

		if (timeout_us > ~0ULL - now) {
			deadline = ~0ULL;
		} else {
			deadline = now + timeout_us;
		}
	}

	flags = spin_lock_irqsave(&sched_lock);
	if (current->wake_pending > 0) {
		current->wake_pending--;
		spin_unlock_irqrestore(&sched_lock, flags);
		if (signal_pending()) {
			return -EINTR;
		}
		return 0;
	}

	current->sleep_timed_out = 0;
	current->sleep_deadline_us = deadline;
	current->state = TASK_SLEEPING;
	schedule_locked();
	spin_unlock_irqrestore(&sched_lock, flags);

	current->sleep_deadline_us = 0;
	if (signal_pending()) {
		return -EINTR;
	}
	if (current->sleep_timed_out) {
		current->sleep_timed_out = 0;
		return -ETIMEDOUT;
	}
	return 0;
}

void sched_consume_wake_pending(struct task *task)
{
	uint64_t flags;

	if (!task) {
		return;
	}

	flags = spin_lock_irqsave(&sched_lock);
	if (task->wake_pending > 0) {
		task->wake_pending--;
	}
	spin_unlock_irqrestore(&sched_lock, flags);
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
		task->sleep_timed_out = 0;
		task->sleep_deadline_us = 0;
		task->state = TASK_RUNNABLE;
		runq_push(task);
	}
	spin_unlock_irqrestore(&sched_lock, flags);
}

static void sched_expire_timed_sleepers(void)
{
	struct task *t;
	uint64_t now = cpu_us_since_boot();

	for (t = all_tasks; t; t = t->all_next) {
		if (t->state != TASK_SLEEPING || t->sleep_deadline_us == 0) {
			continue;
		}
		if (now < t->sleep_deadline_us) {
			continue;
		}
		t->sleep_timed_out = 1;
		t->sleep_deadline_us = 0;
		t->state = TASK_RUNNABLE;
		runq_push(t);
	}
}

void sched_tick(void)
{
	uint64_t flags;

	tick_count++;
	flags = spin_lock_irqsave(&sched_lock);
	sched_expire_timed_sleepers();
	spin_unlock_irqrestore(&sched_lock, flags);

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
			if (nr_tasks > 0) {
				nr_tasks--;
			}
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
