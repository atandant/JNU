/*
 * include/jnu/arch/irq.h — Dynamic interrupt-vector allocator.
 *
 * The IDT has 256 vectors. Architectural exceptions (0–31), the legacy
 * ISA block (32–47), and the LAPIC fixed vectors are reserved at boot.
 * The remaining range [IRQ_DYN_BASE, IRQ_DYN_TOP] is handed out at
 * runtime to MSI / MSI-X consumers via a bitmap allocator.
 *
 * The API is single-CPU today but takes a `cpu` argument so the move to
 * per-CPU vector space under SMP needs no signature change. Only the
 * boot CPU (cpu 0) is supported for now.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/arch/idt.h>
#include <jnu/base/types.h>

/*
 * Allocatable range. Below IRQ_DYN_BASE live the exceptions and the
 * legacy ISA vectors; above IRQ_DYN_TOP live the LAPIC IPI / spurious
 * vectors (VEC_RESCHED_IPI, VEC_SPURIOUS).
 */
#define IRQ_DYN_BASE 0x30u
#define IRQ_DYN_TOP 0xEFu

/* Initialize the allocator: mark reserved vectors as in-use. */
void irq_init(void);

/*
 * Allocate one free vector on `cpu` and install `handler` for it.
 * Writes the chosen vector to *out_vec. Returns 0 on success,
 * -ENOSPC if no vector is free, -EINVAL on bad arguments, or -ENOSYS
 * for an unsupported CPU.
 */
int irq_alloc_vector(unsigned cpu, irq_handler_t handler, uint8_t *out_vec);

/*
 * Allocate `count` vectors on `cpu`. For count == 1 this is identical
 * to irq_alloc_vector. For count > 1 the vectors are contiguous and
 * aligned to the next power of two of `count` (multi-vector MSI); that
 * path is not implemented yet and returns -ENOSYS. Writes the base
 * vector to *out_base.
 */
int irq_alloc_vectors(unsigned cpu, unsigned count, irq_handler_t handler,
		      uint8_t *out_base);

/* Release a previously allocated vector and clear its handler. */
void irq_free_vector(unsigned cpu, uint8_t vec);

int irq_selftest(void);
