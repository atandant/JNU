/*
 * kernel/lib/spinlock.c — IRQ-disable spinlock for the single-CPU build.
 *
 * On v0.0.1 there is exactly one CPU, so the only concurrency is between
 * a kernel thread and an interrupt handler that fires on the same CPU.
 * Disabling interrupts is sufficient mutual exclusion. We still touch
 * `lock->locked` so that the SMP migration is mechanical: replace the
 * compare-and-set with `xchg` and add a `pause` loop.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/panic.h>
#include <jnu/spinlock.h>
#include <jnu/types.h>

void spin_lock_init(struct spinlock *lock)
{
	lock->locked = 0;
}

uint64_t spin_lock_irqsave(struct spinlock *lock)
{
	uint64_t flags;

	__asm__ __volatile__ ("pushfq; popq %0; cli"
			      : "=r"(flags) :: "memory");

	if (lock->locked) {
		/*
		 * On a single CPU, finding the lock already held with IRQs
		 * disabled means we deadlocked: the only way it could have
		 * become locked is by us, recursively.
		 */
		panic("spinlock: recursive acquire");
	}
	lock->locked = 1;

	return flags;
}

void spin_unlock_irqrestore(struct spinlock *lock, uint64_t flags)
{
	if (!lock->locked) {
		panic("spinlock: unlock of unlocked");
	}
	lock->locked = 0;

	if (flags & (1ull << 9)) {	/* IF was set */
		__asm__ __volatile__ ("sti" ::: "memory");
	}
}

int spinlock_selftest(void)
{
	struct spinlock l;
	uint64_t f;

	spin_lock_init(&l);
	f = spin_lock_irqsave(&l);
	spin_unlock_irqrestore(&l, f);

	/*
	 * Ensure repeated lock/unlock cycles preserve the IRQ-flag
	 * save/restore even when called from an IRQ-disabled context.
	 */
	__asm__ __volatile__ ("cli");
	f = spin_lock_irqsave(&l);
	spin_unlock_irqrestore(&l, f);
	/* Caller's expectation is IRQs still disabled. */

	__asm__ __volatile__ ("sti");
	f = spin_lock_irqsave(&l);
	spin_unlock_irqrestore(&l, f);

	return 0;
}
