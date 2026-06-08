/*
 * kernel/lib/mutex.c — Sleeping mutual exclusion lock.
 *
 * Implementation strategy:
 *   A short spinlock (struct spinlock) protects the owner pointer and
 *   the FIFO waiter list.  The spinlock is held only for the few
 *   instructions that inspect/mutate state — never across a sleep.
 *
 *   When a task finds the mutex held, it appends a stack-local
 *   `struct mutex_waiter` to the FIFO, drops the spinlock, and calls
 *   sched_sleep_current().  The unlock path pops the head waiter and
 *   calls sched_wake() to make it runnable.  The woken task resumes
 *   inside mutex_lock(), observes that it is now the owner (set by
 *   the unlocker before waking), and returns.
 *
 *   This design avoids the thundering-herd problem: only one waiter
 *   is woken per unlock, and it is guaranteed to be the next owner.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/base/types.h>
#include <jnu/kernel/panic.h>
#include <jnu/kernel/sched.h>
#include <jnu/lib/klog.h>
#include <jnu/lib/mutex.h>
#include <uapi/jnu/errno.h>

/*
 * Per-waiter node.  Lives on the blocked task's kernel stack for the
 * duration of the sleep — no heap allocation needed.
 */
struct mutex_waiter {
	struct task *task;
	struct mutex_waiter *next;
};

void mutex_init(struct mutex *m)
{
	spin_lock_init(&m->guard);
	m->owner = NULL;
	m->waiters_head = NULL;
	m->waiters_tail = NULL;
}

void mutex_lock(struct mutex *m)
{
	struct task *self = sched_current();
	uint64_t flags;

	if (!self) {
		panic("mutex_lock: no current task");
	}

	flags = spin_lock_irqsave(&m->guard);

	if (m->owner == self) {
		panic("mutex_lock: recursive acquire by tid=%d '%s'", self->tid,
		      self->name ? self->name : "?");
	}

	if (!m->owner) {
		/* Fast path: uncontended. */
		m->owner = self;
		spin_unlock_irqrestore(&m->guard, flags);
		return;
	}

	/*
	 * Slow path: mutex is held.  Enqueue ourselves and sleep.
	 *
	 * The waiter struct lives on our kernel stack.  It is valid
	 * for the entire duration of the sleep because we do not
	 * return from this function until we are woken, and the
	 * stack frame persists across the sleep.
	 */
	struct mutex_waiter w = {
	    .task = self,
	    .next = NULL,
	};

	if (m->waiters_tail) {
		m->waiters_tail->next = &w;
	} else {
		m->waiters_head = &w;
	}
	m->waiters_tail = &w;

	/*
	 * Drop the guard BEFORE sleeping.  The unlock path will set
	 * m->owner = w.task and call sched_wake(&w.task) while
	 * holding the guard, so by the time we wake up we are already
	 * the owner.
	 */
	spin_unlock_irqrestore(&m->guard, flags);

	/*
	 * Sleep until the unlocker wakes us.  On return we are the
	 * owner — the unlocker set m->owner = self before waking us.
	 * We must loop and re-check because sched_sleep_current()
	 * can return spuriously (e.g. signal delivery in the future).
	 */
	for (;;) {
		sched_sleep_current();

		flags = spin_lock_irqsave(&m->guard);
		if (m->owner == self) {
			spin_unlock_irqrestore(&m->guard, flags);
			return;
		}
		spin_unlock_irqrestore(&m->guard, flags);
	}
}

void mutex_unlock(struct mutex *m)
{
	struct task *self = sched_current();
	struct mutex_waiter *next;
	uint64_t flags;

	flags = spin_lock_irqsave(&m->guard);

	if (m->owner != self) {
		panic("mutex_unlock: not owner (owner tid=%d, caller tid=%d)",
		      m->owner ? m->owner->tid : -1, self ? self->tid : -1);
	}

	next = m->waiters_head;
	if (next) {
		/*
		 * Hand ownership directly to the next waiter BEFORE
		 * waking it.  This prevents a third task from stealing
		 * the lock between the wake and the waiter's re-check.
		 */
		m->waiters_head = next->next;
		if (!m->waiters_head) {
			m->waiters_tail = NULL;
		}
		m->owner = next->task;
		spin_unlock_irqrestore(&m->guard, flags);

		sched_wake(next->task);
	} else {
		m->owner = NULL;
		spin_unlock_irqrestore(&m->guard, flags);
	}
}

int mutex_trylock(struct mutex *m)
{
	struct task *self = sched_current();
	uint64_t flags;
	int ret;

	flags = spin_lock_irqsave(&m->guard);

	if (!m->owner) {
		m->owner = self;
		ret = 0;
	} else {
		ret = -EBUSY;
	}

	spin_unlock_irqrestore(&m->guard, flags);
	return ret;
}

bool mutex_is_locked(struct mutex *m)
{
	/* Single read — no lock needed for informational query. */
	return m->owner != NULL;
}

/* ------------------------------------------------------------------ */
/* Selftest                                                           */
/* ------------------------------------------------------------------ */

/*
 * Selftest strategy: two kernel threads contend on the same mutex,
 * each incrementing a shared counter inside the critical section.
 * If the mutex is broken (e.g. both hold it), the counter will
 * over- or under-count.
 *
 * We also test trylock and the recursive-acquire panic guard (via
 * a simple check, not by actually panicking).
 */

#define MUTEX_TEST_ITERS 64

struct mutex_test_ctx {
	struct mutex *mtx;
	volatile int *counter;
	volatile int done;
};

static void mutex_test_thread(void *arg)
{
	struct mutex_test_ctx *ctx = arg;

	for (int i = 0; i < MUTEX_TEST_ITERS; i++) {
		mutex_lock(ctx->mtx);
		(*ctx->counter)++;
		/*
		 * Yield inside the critical section to maximise the
		 * chance of exposing races.
		 */
		sched_yield();
		mutex_unlock(ctx->mtx);
	}

	ctx->done = 1;
}

int mutex_selftest(void)
{
	struct mutex mtx;
	volatile int counter = 0;
	int err;

	mutex_init(&mtx);

	/* ---- basic lock/unlock ---- */
	mutex_lock(&mtx);
	if (!mutex_is_locked(&mtx)) {
		pr_err("mutex: selftest: not locked after lock()\n");
		return -EINVAL;
	}
	mutex_unlock(&mtx);
	if (mutex_is_locked(&mtx)) {
		pr_err("mutex: selftest: still locked after unlock()\n");
		return -EINVAL;
	}

	/* ---- trylock ---- */
	err = mutex_trylock(&mtx);
	if (err) {
		pr_err("mutex: selftest: trylock on free mutex failed\n");
		return err;
	}
	err = mutex_trylock(&mtx);
	if (err != -EBUSY) {
		pr_err("mutex: selftest: trylock on held mutex != -EBUSY\n");
		mutex_unlock(&mtx);
		return -EINVAL;
	}
	mutex_unlock(&mtx);

	/* ---- contended ---- */
	struct mutex_test_ctx ctx_a = {
	    .mtx = &mtx,
	    .counter = &counter,
	    .done = 0,
	};
	struct mutex_test_ctx ctx_b = {
	    .mtx = &mtx,
	    .counter = &counter,
	    .done = 0,
	};

	err = sched_create_kernel_thread("mutex-test-a", mutex_test_thread,
					 (void *)&ctx_a, NULL);
	if (err) {
		return err;
	}
	err = sched_create_kernel_thread("mutex-test-b", mutex_test_thread,
					 (void *)&ctx_b, NULL);
	if (err) {
		return err;
	}

	while (!ctx_a.done || !ctx_b.done) {
		sched_yield();
	}

	int expected = MUTEX_TEST_ITERS * 2;
	if (counter != expected) {
		pr_err("mutex: selftest: counter=%d expected=%d\n", counter,
		       expected);
		return -EINVAL;
	}

	return 0;
}
