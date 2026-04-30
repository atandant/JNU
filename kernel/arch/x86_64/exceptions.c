/*
 * kernel/arch/x86_64/exceptions.c — Architectural exception handlers.
 *
 * Every architectural fault (vectors 0–31) lands in `exceptions_handle`.
 * Most faults are unrecoverable kernel bugs and panic immediately.
 *
 * #PF (vector 14) is the one exception: a fault that originated in user
 * mode kills the offending process rather than halting the machine.  This
 * satisfies spec §15.3 criterion 9.  A fault from kernel mode still panics,
 * because it indicates a kernel bug (bad pointer, broken VMA accounting,
 * or a missed usercopy boundary).
 *
 * x86-64 #PF error-code bits:
 *   bit 0 (P)   — 0 = not-present,  1 = protection violation
 *   bit 1 (W/R) — 0 = read,         1 = write
 *   bit 2 (U/S) — 0 = kernel mode,  1 = user mode
 *   bit 3 (RSVD)— reserved-bit set in PTE
 *   bit 4 (I/D) — instruction fetch
 *   bit 5 (PK)  — protection-key violation
 *   bit 6 (SS)  — shadow-stack access
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/idt.h>
#include <jnu/klog.h>
#include <jnu/panic.h>
#include <jnu/process.h>
#include <jnu/sched.h>
#include <jnu/types.h>

/* Error-code bit positions for #PF (Intel SDM Vol. 3A §4.7). */
#define PF_EC_P		(1u << 0)	/* page present */
#define PF_EC_W		(1u << 1)	/* write access */
#define PF_EC_U		(1u << 2)	/* user-mode access */
#define PF_EC_RSVD	(1u << 3)	/* reserved bit in PTE */
#define PF_EC_ID	(1u << 4)	/* instruction fetch */

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

/*
 * Read CR2 (the faulting virtual address) without modifying any GPRs
 * that the caller's frame cares about.
 */
static inline uint64_t read_cr2(void)
{
	uint64_t cr2;
	__asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
	return cr2;
}

/*
 * Handle a #PF that originated from user mode (error_code bit 2 set,
 * CS CPL == 3).  We kill the current process instead of panicking the
 * whole machine.  The fault address (CR2) and access type are logged so
 * the developer can see what went wrong during bring-up.
 *
 * This intentionally does not implement demand paging: there is no page-in
 * path.  A not-present fault in user space means the process accessed an
 * unmapped address, which is always fatal in v0.0.2 (see debate notes in
 * jnuspec2.md: demand paging is deferred to v0.0.2.1).
 */
static void handle_user_page_fault(struct cpu_state *st)
{
	uint64_t cr2 = read_cr2();
	uint32_t ec  = (uint32_t)st->error_code;
	const char *reason;

	if (ec & PF_EC_RSVD) {
		reason = "reserved-bit in PTE";
	} else if (ec & PF_EC_P) {
		reason = (ec & PF_EC_ID) ? "exec on non-exec page"
		                         : (ec & PF_EC_W) ? "write to read-only page"
		                                          : "read of non-readable page";
	} else {
		reason = (ec & PF_EC_ID) ? "exec of unmapped page"
		                         : (ec & PF_EC_W) ? "write to unmapped page"
		                                          : "read of unmapped page";
	}

	pr_err("pagefault: user %s at 0x%016lx (rip=0x%016lx ec=0x%x)\n",
	       reason, (unsigned long)cr2, (unsigned long)st->rip, ec);

	/*
	 * Kill the process.  process_exit_current() marks it ZOMBIE and
	 * wakes its parent; sched_yield() switches away immediately so we
	 * never return to the faulting instruction.
	 */
	process_exit_current(-1);
	sched_yield();

	/*
	 * sched_yield() does not return if there is another runnable task.
	 * If the scheduler has nothing to run (unlikely during bring-up but
	 * possible in degenerate tests), fall through to a panic so we do
	 * not spin forever.
	 */
	pr_panic("pagefault: no task to schedule after killing pid\n");
	panic_with_state(st);
}

void exceptions_handle(struct cpu_state *st)
{
	/*
	 * #PF gets special treatment: user-mode faults kill the process,
	 * kernel-mode faults panic (they are kernel bugs).
	 *
	 * We check both the error-code U/S bit AND the saved CS CPL to
	 * guard against a kernel-mode fault that somehow has bit 2 set
	 * (e.g. reserved-bit injection via a crafted PTE).
	 */
	if (st->vector == 14) {
		bool from_user = ((st->error_code & PF_EC_U) != 0) &&
		                 ((st->cs & 3) == 3);
		if (from_user) {
			handle_user_page_fault(st);
			/* handle_user_page_fault() does not return. */
		}

		/* Kernel-mode page fault — log CR2 before panicking. */
		pr_panic("kernel pagefault at 0x%016lx (rip=0x%016lx ec=0x%lx)\n",
		         (unsigned long)read_cr2(),
		         (unsigned long)st->rip,
		         (unsigned long)st->error_code);
		panic_with_state(st);
	}

	pr_panic("exception: %s (vector %u) error=0x%lx rip=0x%lx\n",
		 vector_name(st->vector),
		 (unsigned)st->vector,
		 (unsigned long)st->error_code,
		 (unsigned long)st->rip);

	panic_with_state(st);
}

