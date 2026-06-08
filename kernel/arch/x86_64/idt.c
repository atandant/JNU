/*
 * kernel/arch/x86_64/idt.c — Build and install the IDT.
 *
 * Pulls the per-vector stub addresses from `isr_table` (defined in
 * isr.S), wraps them in 64-bit interrupt-gate descriptors, and runs
 * `lidt`. All vectors are interrupt gates (IF cleared on entry).
 *
 * High-trust exceptions point at IST stacks per §2.4:
 *   #DF  → IST1
 *   #NMI → IST2
 *   #MC  → IST3
 *   #PF  → IST4
 *
 * Reference: Intel SDM Vol. 3 §6.14 (interrupt gates in long mode).
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/arch/gdt.h>
#include <jnu/arch/idt.h>
#include <jnu/base/compiler.h>
#include <jnu/base/types.h>
#include <jnu/drivers/apic.h>
#include <jnu/lib/klog.h>
#include <jnu/lib/string.h>

extern uint64_t isr_table[256];

void exceptions_handle(struct cpu_state *st);

struct __packed idt_entry {
	uint16_t off_lo;
	uint16_t selector;
	uint8_t ist;	   /* low 3 bits */
	uint8_t type_attr; /* P | DPL | 0 | type */
	uint16_t off_mid;
	uint32_t off_hi;
	uint32_t zero;
};

struct __packed idt_descriptor {
	uint16_t limit;
	uint64_t base;
};

static struct idt_entry idt[256] __aligned(16);

#define IDT_TYPE_INT_GATE 0x0E
#define IDT_PRESENT (1u << 7)
#define IDT_DPL0 (0u << 5)

static irq_handler_t handlers[256];

static void set_gate(uint8_t vec, uint64_t handler, uint8_t ist)
{
	struct idt_entry *e = &idt[vec];

	e->off_lo = (uint16_t)(handler & 0xFFFF);
	e->selector = GDT_KERNEL_CS;
	e->ist = (uint8_t)(ist & 0x07);
	e->type_attr = (uint8_t)(IDT_PRESENT | IDT_DPL0 | IDT_TYPE_INT_GATE);
	e->off_mid = (uint16_t)((handler >> 16) & 0xFFFF);
	e->off_hi = (uint32_t)(handler >> 32);
	e->zero = 0;
}

void idt_set_handler(uint8_t vector, irq_handler_t handler)
{
	handlers[vector] = handler;
}

void idt_init(void)
{
	memset(idt, 0, sizeof(idt));
	memset(handlers, 0, sizeof(handlers));

	for (unsigned i = 0; i < 256; i++) {
		uint8_t ist = IST_NONE;

		switch (i) {
		case 8:
			ist = IST_DF;
			break;
		case 2:
			ist = IST_NMI;
			break;
		case 18:
			ist = IST_MC;
			break;
		case 14:
			ist = IST_PF;
			break;
		default:
			break;
		}

		set_gate((uint8_t)i, isr_table[i], ist);
	}

	struct idt_descriptor idtr = {
	    .limit = sizeof(idt) - 1,
	    .base = (uint64_t)(uintptr_t)idt,
	};

	__asm__ __volatile__("lidt %0" ::"m"(idtr));

	pr_info("idt: 256 vectors loaded\n");
}

/*
 * Common dispatcher entered from isr.S. Vectors 0–31 are CPU
 * exceptions; the rest go to a runtime-installed handler (or are
 * silently EOI'd as spurious if none is registered, except vector
 * 0xFF which is the LAPIC spurious vector and is intentionally
 * silent).
 */
void interrupt_dispatch(struct cpu_state *st)
{
	uint64_t v = st->vector;

	if (v < 32) {
		exceptions_handle(st);
		return;
	}

	if (v < 256 && handlers[v]) {
		handlers[v](st);
		return;
	}

	if (v == VEC_SPURIOUS) {
		return;
	}

	pr_warn("irq: unhandled vector %u\n", (unsigned)v);
}
