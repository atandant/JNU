/*
 * kernel/arch/x86_64/irq.c — Dynamic interrupt-vector allocator.
 *
 * A 256-bit bitmap tracks which IDT vectors are in use. Vectors
 * outside [IRQ_DYN_BASE, IRQ_DYN_TOP] and the LAPIC timer vector are
 * marked reserved at init so they are never handed out. Allocation of a
 * single vector is a find-first-zero scan (__builtin_ctzll); the
 * handler is installed through the existing IDT handler table.
 *
 * The vector space is per-CPU on x86, but only the boot CPU is
 * supported until SMP lands; the `cpu` argument is validated and
 * reserved for that future.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/arch/idt.h>
#include <jnu/arch/irq.h>
#include <jnu/base/types.h>
#include <jnu/drivers/apic.h>
#include <jnu/lib/klog.h>
#include <jnu/lib/spinlock.h>
#include <uapi/jnu/errno.h>

#define IRQ_NVEC 256u
#define IRQ_BITMAP_WORDS (IRQ_NVEC / 64u)

/* Bit set == vector in use or reserved. */
static uint64_t bitmap[IRQ_BITMAP_WORDS];
static struct spinlock irq_lock = SPINLOCK_INITIALIZER;
static bool irq_ready;

static inline void bit_set(unsigned v)
{
	bitmap[v / 64u] |= (uint64_t)1u << (v % 64u);
}

static inline void bit_clear(unsigned v)
{
	bitmap[v / 64u] &= ~((uint64_t)1u << (v % 64u));
}

static inline bool bit_test(unsigned v)
{
	return (bitmap[v / 64u] >> (v % 64u)) & 1u;
}

void irq_init(void)
{
	uint64_t flags = spin_lock_irqsave(&irq_lock);

	for (unsigned w = 0; w < IRQ_BITMAP_WORDS; w++)
		bitmap[w] = 0;

	/* Reserve everything below the dynamic range (exceptions + ISA). */
	for (unsigned v = 0; v < IRQ_DYN_BASE; v++)
		bit_set(v);

	/* Reserve everything above the dynamic range (IPIs + spurious). */
	for (unsigned v = IRQ_DYN_TOP + 1u; v < IRQ_NVEC; v++)
		bit_set(v);

	/*
	 * The LAPIC timer vector sits inside the dynamic range but is
	 * owned by lapic_timer.c — never hand it out.
	 */
	bit_set(VEC_LAPIC_TIMER);

	irq_ready = true;
	spin_unlock_irqrestore(&irq_lock, flags);

	pr_info("irq: vector allocator ready (dynamic range 0x%x-0x%x)\n",
		(unsigned)IRQ_DYN_BASE, (unsigned)IRQ_DYN_TOP);
}

int irq_alloc_vectors(unsigned cpu, unsigned count, irq_handler_t handler,
		      uint8_t *out_base)
{
	uint64_t flags;
	int ret = -ENOSPC;

	if (cpu != 0)
		return -ENOSYS; /* only the boot CPU has a bitmap today */
	if (count == 0 || !handler || !out_base)
		return -EINVAL;
	if (count > 1)
		return -ENOSYS; /* multi-vector MSI not implemented yet */

	flags = spin_lock_irqsave(&irq_lock);
	if (!irq_ready) {
		spin_unlock_irqrestore(&irq_lock, flags);
		return -EINVAL;
	}

	for (unsigned v = IRQ_DYN_BASE; v <= IRQ_DYN_TOP; v++) {
		if (!bit_test(v)) {
			bit_set(v);
			*out_base = (uint8_t)v;
			ret = 0;
			break;
		}
	}
	spin_unlock_irqrestore(&irq_lock, flags);

	if (ret == 0)
		idt_set_handler(*out_base, handler);
	return ret;
}

int irq_alloc_vector(unsigned cpu, irq_handler_t handler, uint8_t *out_vec)
{
	return irq_alloc_vectors(cpu, 1, handler, out_vec);
}

void irq_free_vector(unsigned cpu, uint8_t vec)
{
	uint64_t flags;

	if (cpu != 0)
		return;
	if (vec < IRQ_DYN_BASE || vec > IRQ_DYN_TOP || vec == VEC_LAPIC_TIMER)
		return;

	/* Stop dispatching to the handler before the vector can be reused. */
	idt_set_handler(vec, NULL);

	flags = spin_lock_irqsave(&irq_lock);
	bit_clear(vec);
	spin_unlock_irqrestore(&irq_lock, flags);
}

static void irq_selftest_handler(struct cpu_state *st) { (void)st; }

int irq_selftest(void)
{
	uint8_t a = 0;
	uint8_t b = 0;
	int err;

	/* A reserved vector must never be allocatable. */
	if (!bit_test(VEC_LAPIC_TIMER)) {
		pr_err("irq_selftest: LAPIC timer vector not reserved\n");
		return -EIO;
	}

	/* Two allocations must yield distinct, in-range vectors. */
	err = irq_alloc_vector(0, irq_selftest_handler, &a);
	if (err) {
		pr_err("irq_selftest: alloc a failed (%d)\n", err);
		return err;
	}
	err = irq_alloc_vector(0, irq_selftest_handler, &b);
	if (err) {
		irq_free_vector(0, a);
		pr_err("irq_selftest: alloc b failed (%d)\n", err);
		return err;
	}
	if (a == b || a < IRQ_DYN_BASE || a > IRQ_DYN_TOP || b < IRQ_DYN_BASE ||
	    b > IRQ_DYN_TOP) {
		irq_free_vector(0, a);
		irq_free_vector(0, b);
		pr_err("irq_selftest: bad vectors a=%u b=%u\n", a, b);
		return -EIO;
	}

	/* Freeing must make the vector available again. */
	irq_free_vector(0, a);
	if (bit_test(a)) {
		irq_free_vector(0, b);
		pr_err("irq_selftest: free did not release vector %u\n", a);
		return -EIO;
	}

	irq_free_vector(0, b);
	pr_info("irq_selftest: alloc/free OK\n");
	return 0;
}
