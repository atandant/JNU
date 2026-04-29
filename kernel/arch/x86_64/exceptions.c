/*
 * kernel/arch/x86_64/exceptions.c — Architectural exception handlers.
 *
 * Every architectural fault (vectors 0–31) lands in `exceptions_handle`,
 * which formats a panic with the canonical decode and routes through
 * `panic_with_state` to produce the §13 forensic dump. We deliberately
 * do not try to recover from any of these in v0.0.1: the kernel either
 * works or panics with a clean trace.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/idt.h>
#include <jnu/klog.h>
#include <jnu/panic.h>
#include <jnu/types.h>

static const char *vector_name(uint64_t v)
{
	switch (v) {
	case 0:		return "#DE divide error";
	case 1:		return "#DB debug";
	case 2:		return "#NMI";
	case 3:		return "#BP breakpoint";
	case 4:		return "#OF overflow";
	case 5:		return "#BR bound range";
	case 6:		return "#UD invalid opcode";
	case 7:		return "#NM device-not-available";
	case 8:		return "#DF double fault";
	case 9:		return "(reserved)";
	case 10:	return "#TS invalid TSS";
	case 11:	return "#NP segment-not-present";
	case 12:	return "#SS stack-segment fault";
	case 13:	return "#GP general protection";
	case 14:	return "#PF page fault";
	case 16:	return "#MF x87 fp error";
	case 17:	return "#AC alignment check";
	case 18:	return "#MC machine check";
	case 19:	return "#XF SIMD fp exception";
	case 20:	return "#VE virtualization";
	case 21:	return "#CP control-protection";
	default:	return "(unknown)";
	}
}

void exceptions_handle(struct cpu_state *st)
{
	pr_panic("exception: %s (vector %u) error=0x%lx rip=0x%lx\n",
		 vector_name(st->vector),
		 (unsigned)st->vector,
		 (unsigned long)st->error_code,
		 (unsigned long)st->rip);

	panic_with_state(st);
}
