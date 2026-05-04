/*
 * kernel/arch/x86_64/exceptions.c — Architectural exception handlers.
 *
 * Every architectural fault (vectors 0–31) lands in `exceptions_handle`.
 *
 * User-mode faults (CPL 3) kill the offending process and schedule away.
 * This satisfies spec §15.3 criterion 9 ("user page fault kills the user
 * process") and extends the same policy to #GP, #UD, #DE, etc. so that
 * no user-mode bug can panic the kernel.
 *
 * Kernel-mode faults panic — they always indicate a kernel bug.
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
#include <jnu/paging.h>
#include <jnu/panic.h>
#include <jnu/process.h>
#include <jnu/sched.h>
#include <jnu/types.h>
#include <jnu/vma.h>
#include <jnu/vmm.h>

/* Error-code bit positions for #PF (Intel SDM Vol. 3A §4.7). */
#define PF_EC_P (1u << 0)    /* page present */
#define PF_EC_W (1u << 1)    /* write access */
#define PF_EC_U (1u << 2)    /* user-mode access */
#define PF_EC_RSVD (1u << 3) /* reserved bit in PTE */
#define PF_EC_ID (1u << 4)   /* instruction fetch */

static const char *vector_name(uint64_t v)
{
	switch (v) {
	case 0:
		return "#DE divide error";
	case 1:
		return "#DB debug";
	case 2:
		return "#NMI";
	case 3:
		return "#BP breakpoint";
	case 4:
		return "#OF overflow";
	case 5:
		return "#BR bound range";
	case 6:
		return "#UD invalid opcode";
	case 7:
		return "#NM device-not-available";
	case 8:
		return "#DF double fault";
	case 9:
		return "(reserved)";
	case 10:
		return "#TS invalid TSS";
	case 11:
		return "#NP segment-not-present";
	case 12:
		return "#SS stack-segment fault";
	case 13:
		return "#GP general protection";
	case 14:
		return "#PF page fault";
	case 16:
		return "#MF x87 fp error";
	case 17:
		return "#AC alignment check";
	case 18:
		return "#MC machine check";
	case 19:
		return "#XF SIMD fp exception";
	case 20:
		return "#VE virtualization";
	case 21:
		return "#CP control-protection";
	default:
		return "(unknown)";
	}
}

/*
 * True if the saved trap frame shows a fault that originated from
 * user mode (ring 3).  For #PF we cross-check the error-code U/S bit
 * against the saved CS CPL; for all other vectors only CS matters.
 */
static bool fault_from_user(const struct cpu_state *st)
{
	if ((st->cs & 3) != 3) {
		return false;
	}
	/*
	 * For #PF the CPU sets error_code bit 2 when the fault originated
	 * in user mode. Require both bits to agree as a belt-and-suspenders
	 * guard against a kernel-mode fault with a mangled error code.
	 */
	if (st->vector == 14) {
		return (st->error_code & PF_EC_U) != 0;
	}
	return true;
}

/*
 * Build a human-readable reason string for a #PF from the error code.
 */
static const char *pf_reason(uint32_t ec)
{
	if (ec & PF_EC_RSVD) {
		return "reserved-bit in PTE";
	}
	if (ec & PF_EC_P) {
		return (ec & PF_EC_ID)	? "exec on non-exec page"
		       : (ec & PF_EC_W) ? "write to read-only page"
					: "read of non-readable page";
	}
	return (ec & PF_EC_ID)	? "exec of unmapped page"
	       : (ec & PF_EC_W) ? "write to unmapped page"
				: "read of unmapped page";
}

/*
 * Kill the current user process and schedule away.  Never returns.
 *
 * process_exit_current() handles process-level teardown: closing fds,
 * marking the process ZOMBIE, and waking the parent for waitpid().
 *
 * sched_exit_current() marks the *task* ZOMBIE and context-switches
 * to the next runnable task.  It never returns.  Using sched_yield()
 * here would be a critical bug: the task would still be TASK_RUNNING,
 * and schedule_locked() would re-queue it — creating an infinite
 * fault loop as the CPU endlessly retries the faulting instruction.
 */
static void kill_current_user(void)
{
	process_exit_current(-1);
	sched_exit_current(-1);
	__builtin_unreachable();
}

void exceptions_handle(struct cpu_state *st)
{
	/*
	 * v0.0.3 §2.7: #NM (vector 7) is impossible with eager FPU
	 * save (CR0.TS is always clear).  Panic unconditionally — its
	 * appearance is a CPU/setup bug, not a user fault.
	 */
	if (st->vector == 7) {
		panic("FPU #NM with eager save active "
		      "(rip=0x%lx, cs=0x%lx)",
		      (unsigned long)st->rip, (unsigned long)st->cs);
	}

	/*
	 * User-mode faults: kill the process, do not panic the kernel.
	 * Every exception from ring 3 is fatal to the process, whether
	 * it is #PF, #GP, #UD, #DE, or anything else.
	 */
	if (fault_from_user(st)) {
		if (st->vector == 14) {
			uint64_t cr2 = paging_read_cr2();
			uint32_t ec = (uint32_t)st->error_code;

			/*
			 * CoW resolution: user write to a present page
			 * in a logically-writable VMA.  Try to resolve
			 * before falling through to the kill path.
			 */
			if ((ec & PF_EC_P) && (ec & PF_EC_W)) {
				struct task *t = sched_current();
				struct addr_space *space =
				    t->process ? t->process->space : NULL;

				if (space) {
					struct vma *v = vma_find(
					    &space->vmas, cr2);

					if (v && (v->flags & VMA_WRITE)) {
						int cow_err =
						    vmm_handle_cow_fault(
							space, cr2 & ~PAGE_MASK);
						if (cow_err == 0) {
							return;
						}
					}
				}
			}

			/*
			 * v0.0.3 §2.5: lazy zero-fill for absent pages
			 * inside a valid VMA.  The PTE is not present
			 * (PF_EC_P is clear).
			 */
			if (!(ec & PF_EC_P)) {
				struct task *t = sched_current();
				struct addr_space *space =
				    t->process ? t->process->space : NULL;

				if (space) {
					struct vma *v = vma_find(
					    &space->vmas, cr2);

					if (v) {
						vaddr_t va = cr2 & ~PAGE_MASK;
						int fill_err =
						    vmm_handle_lazy_fault(
							space, v, va, ec);
						if (fill_err == 0) {
							return;
						}
					}
				}
			}

			pr_err("pagefault: user %s at 0x%016lx "
			       "(rip=0x%016lx ec=0x%x)\n",
			       pf_reason(ec), (unsigned long)cr2,
			       (unsigned long)st->rip, ec);
		} else {
			pr_err("user exception: %s (vector %u) "
			       "error=0x%lx rip=0x%lx\n",
			       vector_name(st->vector), (unsigned)st->vector,
			       (unsigned long)st->error_code,
			       (unsigned long)st->rip);
		}
		kill_current_user();
	}

	/*
	 * Kernel-mode faults: always panic.
	 * For #PF, include CR2 in the output.
	 */
	if (st->vector == 14) {
		pr_panic("kernel pagefault at 0x%016lx "
			 "(rip=0x%016lx ec=0x%lx)\n",
			 (unsigned long)paging_read_cr2(),
			 (unsigned long)st->rip, (unsigned long)st->error_code);
		panic_with_state(st);
	}

	pr_panic("exception: %s (vector %u) error=0x%lx rip=0x%lx\n",
		 vector_name(st->vector), (unsigned)st->vector,
		 (unsigned long)st->error_code, (unsigned long)st->rip);

	panic_with_state(st);
}
