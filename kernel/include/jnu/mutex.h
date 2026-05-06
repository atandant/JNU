/*
 * include/jnu/mutex.h — Sleeping mutual exclusion lock.
 *
 * A mutex is a sleeping lock: when contended the caller blocks via
 * sched_sleep_current() instead of spinning.  This makes it suitable
 * for code paths that can sleep (I/O, page faults, long critical
 * sections).  Unlike spinlocks, mutexes must NOT be held in IRQ
 * context — the sleep path is undefined with interrupts disabled.
 *
 * Semantics:
 *   - Only one task can hold the mutex at a time.
 *   - The holder MUST be the one to unlock it (owner check).
 *   - Recursive locking panics (deadlock on single-CPU).
 *   - mutex_trylock() returns 0 on success, -EBUSY if held.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/spinlock.h>
#include <jnu/types.h>

struct task;

struct mutex {
	/*
	 * Internal spinlock protects the owner pointer and the
	 * waiter list.  Held only for the few instructions needed
	 * to inspect/update the state — never across a sleep.
	 */
	struct spinlock guard;
	struct task *owner;

	/*
	 * FIFO waiter queue: tasks that called mutex_lock()
	 * while the mutex was held append themselves here and sleep.
	 * mutex_unlock() pops the head and wakes it.
	 */
	struct mutex_waiter *waiters_head;
	struct mutex_waiter *waiters_tail;
};

#define MUTEX_INITIALIZER                                                      \
	{                                                                      \
	    .guard = SPINLOCK_INITIALIZER,                                     \
	    .owner = NULL,                                                     \
	    .waiters_head = NULL,                                              \
	    .waiters_tail = NULL,                                              \
	}

void mutex_init(struct mutex *m);

/*
 * Acquire the mutex.  Sleeps if the mutex is already held by another
 * task; returns with the lock held and the caller recorded as owner.
 * Must NOT be called with interrupts disabled or from IRQ context.
 */
void mutex_lock(struct mutex *m);

/*
 * Release the mutex.  Wakes the first waiter (if any).  Panics if
 * the caller is not the current owner.
 */
void mutex_unlock(struct mutex *m);

/*
 * Non-blocking acquire.  Returns 0 on success, -EBUSY if the mutex
 * is already held.  Never sleeps.
 */
int mutex_trylock(struct mutex *m);

/*
 * Returns true if the mutex is currently held (by any task).
 * Informational only — the value may be stale by the time the caller
 * acts on it.
 */
bool mutex_is_locked(struct mutex *m);

int mutex_selftest(void);
