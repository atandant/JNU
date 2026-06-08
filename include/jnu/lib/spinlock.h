/*
 * include/jnu/lib/spinlock.h — IRQ-disable spinlock (single-CPU shim).
 *
 * One spinlock primitive. On the single-CPU build it saves and restores
 * IRQ flags around `cli`/`sti`; the lock-acquire side is a no-op since
 * there is no concurrent CPU. When SMP arrives the body becomes a real
 * `xchg`/`pause` loop without API change.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/base/types.h>

struct spinlock {
	volatile uint32_t locked;
};

#define SPINLOCK_INITIALIZER {.locked = 0}

void spin_lock_init(struct spinlock *lock);

/*
 * Acquire the lock with interrupts disabled. Returns the saved RFLAGS
 * to be passed back to spin_unlock_irqrestore.
 */
uint64_t spin_lock_irqsave(struct spinlock *lock);

void spin_unlock_irqrestore(struct spinlock *lock, uint64_t flags);

int spinlock_selftest(void);
