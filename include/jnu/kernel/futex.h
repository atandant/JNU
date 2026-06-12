/*
 * include/jnu/kernel/futex.h — In-kernel futex primitives.
 *
 * A futex ("fast userspace mutex") is a 32-bit word in user memory that
 * the kernel can block on (FUTEX_WAIT) and wake from (FUTEX_WAKE). It is
 * the foundation musl libc builds every pthread synchronization object
 * on (mutex, cond, once, barrier, join).
 *
 * Keying: JNU only shares memory between threads of the same group
 * (CLONE_VM), so every futex is process-private. A futex is identified
 * by the pair (struct process *, user virtual address) — see
 * kernel/kernel/futex.c. This means a futex word at the same address in
 * two different processes is two distinct futexes, which is correct
 * because those processes do not share that page.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/base/types.h>

/*
 * Block the calling task until *uaddr is woken or (optionally) a
 * timeout elapses.
 *
 *   - Atomically (w.r.t. wakers) checks that *uaddr == val; if it does
 *     not match, returns -EAGAIN without sleeping. This closes the race
 *     where the value changed after userspace's own check but before
 *     entering the kernel.
 *   - timeout_us == 0 means wait indefinitely. A non-zero timeout is a
 *     relative deadline in microseconds; on expiry returns -ETIMEDOUT.
 *   - A pending death/work flag (TIF_NEED_DIE) unblocks the wait with
 *     -EINTR so exit_group()/fatal signals stay prompt.
 *
 * Returns 0 when woken normally.
 */
int futex_wait(const uint32_t *uaddr, uint32_t val, uint64_t timeout_us);

/*
 * Wake up to `count` tasks blocked on *uaddr. `count` may be INT_MAX to
 * wake all waiters (used by pthread_cond_broadcast / barriers). Returns
 * the number of tasks woken (>= 0).
 */
int futex_wake(const uint32_t *uaddr, int count);

/*
 * REQUEUE: wake up to `nr_wake` waiters on *uaddr. JNU does not move
 * waiters between futex queues; instead it additionally wakes up to
 * `nr_requeue` further waiters on the same word. Waking is always a
 * semantically safe substitute for requeuing — the woken thread simply
 * re-contends for the target lock itself — at the cost of the requeue
 * optimization (a small thundering herd). musl's pthread_cond unlock
 * path relies on this. Returns the number of tasks woken.
 */
int futex_requeue(const uint32_t *uaddr, int nr_wake, int nr_requeue);

int futex_selftest(void);
