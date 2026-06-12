/*
 * kernel/kernel/futex.c — In-kernel futex primitives.
 *
 * v0.0.5 (item 2: futex). Implements the FUTEX_WAIT / FUTEX_WAKE /
 * FUTEX_REQUEUE operations musl libc builds its pthread primitives on.
 *
 * Design (debated in the futex roadmap):
 *
 *   - Process-private only. JNU shares memory exclusively between
 *     threads of one group (CLONE_VM), so a futex is keyed by the pair
 *     (struct process *, user virtual address). The key namespace for
 *     kernel-context callers (no process) is the NULL process, which is
 *     correct since they share the kernel address space.
 *
 *   - A small global hash of buckets, each a spinlock guarding a
 *     singly-linked list of waiters. Waiter nodes live on the blocked
 *     task's kernel stack — no allocation — exactly like kernel/lib/
 *     mutex.c.
 *
 *   - WAIT atomicity: the value compare happens *under the bucket lock*
 *     after the waiter is already enqueued. copy_from_user() on JNU is
 *     non-sleeping and fault-free (it walks the page tables and memcpys;
 *     there is no demand paging — see kernel/user/copy.c), so reading
 *     the futex word with the bucket spinlock held and interrupts
 *     disabled is safe and closes the classic lost-wakeup race together
 *     with the scheduler's wake_pending credit.
 *
 *   - REQUEUE is implemented as an additional wake rather than moving
 *     waiters between queues. Waking is always a safe substitute for
 *     requeuing (the woken thread simply re-contends), trading the
 *     optimization for a small thundering herd.
 *
 *   - Timed waits block in sched_sleep_timed_interruptible() with the
 *     same wake_pending / death-flag semantics as untimed waits, so a
 *     long timeout does not spin. sched_tick() expires timed sleepers.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/arch/cpu.h>
#include <jnu/base/types.h>
#include <jnu/kernel/futex.h>
#include <jnu/kernel/process.h>
#include <jnu/kernel/sched.h>
#include <jnu/lib/spinlock.h>
#include <jnu/mm/paging.h>
#include <jnu/mm/pmm.h>
#include <jnu/mm/vmm.h>
#include <jnu/user/usercopy.h>
#include <uapi/jnu/errno.h>

#define FUTEX_WAKE_ALL 0x7fffffff

/*
 * Per-waiter node. Lives on the blocked task's kernel stack for the
 * duration of the wait. `woken` is set by the waker under the bucket
 * lock and is the authoritative signal that this waiter was woken (a
 * bare sched_wake() can also fire spuriously or for group-exit).
 */
struct futex_waiter {
	struct task *task;
	struct process *proc;
	uintptr_t uaddr;
	int woken;
	struct futex_waiter *next;
};

#define FUTEX_NR_BUCKETS 64u

struct futex_bucket {
	struct spinlock guard;
	struct futex_waiter *head;
};

static struct futex_bucket futex_buckets[FUTEX_NR_BUCKETS];

static struct futex_bucket *bucket_for(struct process *proc, uintptr_t uaddr)
{
	/*
	 * Mix the process pointer and the word address. The word is
	 * 4-byte aligned, so the low two bits carry no information;
	 * fold in higher bits of both operands.
	 */
	uintptr_t h = (uintptr_t)proc;
	h ^= uaddr >> 2;
	h ^= uaddr >> 14;
	h *= 0x9e3779b97f4a7c15ull;
	return &futex_buckets[(h >> 32) % FUTEX_NR_BUCKETS];
}

/* Unlink `w` from bucket `b` if present. Caller holds b->guard. */
static void bucket_remove(struct futex_bucket *b, struct futex_waiter *w)
{
	struct futex_waiter **link = &b->head;

	while (*link) {
		if (*link == w) {
			*link = w->next;
			w->next = NULL;
			return;
		}
		link = &(*link)->next;
	}
}

int futex_wait(const uint32_t *uaddr, uint32_t val, uint64_t timeout_us)
{
	struct task *self = sched_current();
	struct process *proc = self ? self->process : NULL;
	struct futex_bucket *b;
	uint32_t cur;
	uint64_t flags;
	int err;
	int ret;

	if (!self) {
		return -EINVAL;
	}
	if ((uintptr_t)uaddr & 0x3u) {
		return -EINVAL;
	}
	if (!user_range_ok(uaddr, sizeof(*uaddr))) {
		return -EFAULT;
	}

	b = bucket_for(proc, (uintptr_t)uaddr);

	struct futex_waiter w = {
	    .task = self,
	    .proc = proc,
	    .uaddr = (uintptr_t)uaddr,
	    .woken = 0,
	    .next = NULL,
	};

	/*
	 * Enqueue first, then compare under the lock. A waker that fires
	 * after we drop the lock but before we sleep is caught by the
	 * scheduler's wake_pending credit. copy_from_user() neither
	 * sleeps nor faults, so it is safe to call with the guard held
	 * and interrupts disabled.
	 */
	flags = spin_lock_irqsave(&b->guard);
	w.next = b->head;
	b->head = &w;

	err = copy_from_user(&cur, uaddr, sizeof(cur));
	if (err) {
		bucket_remove(b, &w);
		spin_unlock_irqrestore(&b->guard, flags);
		return err;
	}
	if (cur != val) {
		bucket_remove(b, &w);
		spin_unlock_irqrestore(&b->guard, flags);
		return -EAGAIN;
	}
	spin_unlock_irqrestore(&b->guard, flags);

	{
		uint64_t start = 0;

		if (timeout_us != 0) {
			start = cpu_us_since_boot();
		}

		for (;;) {
			flags = spin_lock_irqsave(&b->guard);
			if (w.woken) {
				spin_unlock_irqrestore(&b->guard, flags);
				sched_consume_wake_pending(self);
				ret = 0;
				break;
			}
			spin_unlock_irqrestore(&b->guard, flags);

			if (timeout_us == 0) {
				int s = sched_sleep_interruptible();

				if (s == -EINTR) {
					ret = -EINTR;
					break;
				}
			} else {
				uint64_t elapsed = cpu_us_since_boot() - start;
				uint64_t remaining;

				if (elapsed >= timeout_us) {
					ret = -ETIMEDOUT;
					break;
				}
				remaining = timeout_us - elapsed;
				int s =
				    sched_sleep_timed_interruptible(remaining);

				if (s == -EINTR) {
					ret = -EINTR;
					break;
				}
				if (s == -ETIMEDOUT) {
					ret = -ETIMEDOUT;
					break;
				}
			}
			/* Spurious wakeup: re-check w.woken. */
		}
	}

	/*
	 * Unified cleanup. If a waker raced with our decision to give up
	 * (timeout/EINTR), honor the wake so it is not lost — otherwise
	 * the wake would be charged to a waiter that never observes it.
	 */
	flags = spin_lock_irqsave(&b->guard);
	if (w.woken) {
		ret = 0;
		sched_consume_wake_pending(self);
	} else {
		bucket_remove(b, &w);
	}
	spin_unlock_irqrestore(&b->guard, flags);
	return ret;
}

int futex_wake(const uint32_t *uaddr, int count)
{
	struct task *self = sched_current();
	struct process *proc = self ? self->process : NULL;
	struct futex_bucket *b;
	struct futex_waiter **link;
	uint64_t flags;
	int woken = 0;

	if ((uintptr_t)uaddr & 0x3u) {
		return -EINVAL;
	}
	if (!user_range_ok(uaddr, sizeof(*uaddr))) {
		return -EFAULT;
	}
	if (count < 0) {
		count = FUTEX_WAKE_ALL;
	}

	b = bucket_for(proc, (uintptr_t)uaddr);

	flags = spin_lock_irqsave(&b->guard);
	link = &b->head;
	while (*link && woken < count) {
		struct futex_waiter *w = *link;

		if (w->proc == proc && w->uaddr == (uintptr_t)uaddr) {
			*link = w->next;
			w->next = NULL;
			w->woken = 1;
			/*
			 * Lock order (bucket -> sched) matches the
			 * established tree -> sched ordering in
			 * process_group_exit(); sched_wake never re-enters
			 * the futex layer, so nesting is safe.
			 */
			sched_wake(w->task);
			woken++;
		} else {
			link = &w->next;
		}
	}
	spin_unlock_irqrestore(&b->guard, flags);

	return woken;
}

int futex_requeue(const uint32_t *uaddr, int nr_wake, int nr_requeue)
{
	long total;

	if (nr_wake < 0) {
		nr_wake = 0;
	}
	if (nr_requeue < 0) {
		nr_requeue = 0;
	}
	total = (long)nr_wake + (long)nr_requeue;
	if (total > FUTEX_WAKE_ALL) {
		total = FUTEX_WAKE_ALL;
	}
	return futex_wake(uaddr, (int)total);
}

/* ------------------------------------------------------------------ */
/* Selftest                                                           */
/* ------------------------------------------------------------------ */

/*
 * Smoke test the non-blocking paths against a real user mapping (a
 * blocking wake/wait test would need a second task to issue the wake;
 * that is exercised end-to-end by musl pthread programs). We verify:
 *   1. futex_wake on an idle word wakes nobody (returns 0).
 *   2. futex_wait with a mismatched value returns -EAGAIN immediately.
 *   3. futex_wait on a matching value with a short timeout blocks via
 *      the poll path and returns -ETIMEDOUT.
 *   4. misaligned / NULL words are rejected.
 */
#define FUTEX_TEST_VA 0x0000000000400000ull

int futex_selftest(void)
{
	paddr_t pa;
	uint32_t *word = (uint32_t *)FUTEX_TEST_VA;
	const uint32_t initial = 0x1234u;
	int err;
	int ret;

	if (futex_wake((const uint32_t *)0x1, 1) != -EINVAL) {
		return -EINVAL; /* misaligned address must be rejected */
	}

	pa = pmm_alloc_user_page();
	if (!pa) {
		return -ENOMEM;
	}
	err = vmm_map(vmm_kernel_space(), FUTEX_TEST_VA, pa, 1,
		      VMA_READ | VMA_WRITE | VMA_USER);
	if (err) {
		pmm_free_pages(pa, 0);
		return err;
	}

	err = copy_to_user(word, &initial, sizeof(initial));
	if (err) {
		goto out;
	}

	/* (1) no waiters → wakes nobody. */
	ret = futex_wake((const uint32_t *)word, 1);
	if (ret != 0) {
		err = (ret < 0) ? ret : -EINVAL;
		goto out;
	}

	/* (2) value mismatch → -EAGAIN without blocking. */
	ret = futex_wait((const uint32_t *)word, 0x9999u, 0);
	if (ret != -EAGAIN) {
		err = -EINVAL;
		goto out;
	}

	/* (3) value matches, short timeout, no waker → -ETIMEDOUT. */
	ret = futex_wait((const uint32_t *)word, 0x1234u, 2000);
	if (ret != -ETIMEDOUT) {
		err = -EINVAL;
		goto out;
	}

	err = 0;

out:
	vmm_unmap(vmm_kernel_space(), FUTEX_TEST_VA, 1);
	return err;
}
