/*
 * include/jnu/apic.h — Local APIC + IOAPIC + minimal ACPI MADT parser.
 *
 * v0.0.1 uses xAPIC (MMIO) mode. We discover the LAPIC base via the
 * ACPI MADT (cross-checked against IA32_APIC_BASE). The MADT also
 * tells us the IOAPIC list and any ISA IRQ overrides (edge/level,
 * polarity, redirected source).
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>

/* Vector layout, per §2.4. */
#define VEC_TIMER		32	/* PIT in v0.0.1 */
#define VEC_KBD			33
#define VEC_COM1		34
#define VEC_RESCHED_IPI		254
#define VEC_SPURIOUS		255

void apic_init(uint64_t rsdp_phys, uint64_t hhdm_offset);

void apic_eoi(void);

/*
 * Configure an IOAPIC redirection entry for an ISA IRQ. Honors any
 * MADT override (changing the global system interrupt, or flipping
 * edge/level / polarity).
 */
void ioapic_route_isa_irq(uint8_t isa_irq, uint8_t vector);

void ioapic_mask(uint8_t isa_irq);
void ioapic_unmask(uint8_t isa_irq);
