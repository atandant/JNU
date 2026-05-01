/*
 * include/jnu/idt.h — Interrupt descriptor table and trap frame.
 *
 * 256 vectors, all interrupt gates (IF cleared on entry). The error
 * code asymmetry (vectors 8/10/11/12/13/14/17/21/29/30 push hardware
 * codes; everything else pushes a fake zero) is handled by the NASM
 * stubs in `isr.S`. All stubs jump to `isr_common`, which saves all
 * GPRs into a `struct cpu_state` on the stack and tail-calls
 * `interrupt_dispatch`.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/compiler.h>
#include <jnu/types.h>

/*
 * The trap frame as built by isr.S. Field order matches the push order
 * in the assembly — do not reorder without also patching isr.S.
 */
struct cpu_state {
	/* Pushed by isr_common, in this order: */
	uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
	uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

	/* Pushed by the per-vector stub: */
	uint64_t vector;
	uint64_t error_code; /* fake 0 if the CPU did not push one */

	/* Pushed by the CPU: */
	uint64_t rip;
	uint64_t cs;
	uint64_t rflags;
	uint64_t rsp;
	uint64_t ss;
};

typedef void (*irq_handler_t)(struct cpu_state *st);

void idt_init(void);

/*
 * Install a runtime handler for a vector. Used by IOAPIC users (PS/2,
 * COM1 RX) and by the LAPIC timer in Phase 3+.
 */
void idt_set_handler(uint8_t vector, irq_handler_t handler);

/* Common dispatch entry from isr.S. */
void interrupt_dispatch(struct cpu_state *st);

/* Architectural-exception (vectors 0–31) entry, called from interrupt_dispatch.
 */
void exceptions_handle(struct cpu_state *st);
