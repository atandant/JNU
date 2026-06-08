/*
 * kernel/arch/x86_64/arch_syscall.c - syscall/sysret MSR setup.
 *
 * Owns the per-CPU syscall scratch struct that syscall_entry.S reads
 * via the GS segment after SWAPGS.  v0.0.2 has a single CPU so there
 * is one struct; the design is laid out so SMP only needs to allocate
 * one per CPU and write each CPU's MSR_KERNEL_GS_BASE accordingly,
 * without touching the asm.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/arch/arch_syscall.h>
#include <jnu/arch/cpu.h>
#include <jnu/arch/gdt.h>
#include <jnu/base/types.h>
#include <jnu/lib/klog.h>

#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084

#define EFER_SCE (1ull << 0)

#define RFLAGS_TF (1ull << 8)
#define RFLAGS_IF (1ull << 9)
#define RFLAGS_DF (1ull << 10)
#define RFLAGS_IOPL (3ull << 12)
#define RFLAGS_NT (1ull << 14)
#define RFLAGS_AC (1ull << 18)

extern void syscall_entry(void);

/*
 * Per-CPU syscall scratch.  Layout MUST match syscall_entry.S:
 *   offset 0 -> saved user RSP
 *   offset 8 -> kernel stack top of the currently-scheduled task
 *
 * Future SMP: replace this with one slot per CPU and have the per-CPU
 * bring-up write its own MSR_KERNEL_GS_BASE.  No asm change required.
 */
struct syscall_scratch {
	uint64_t user_rsp;
	uint64_t kernel_rsp;
	int64_t current_nr;
};

static struct syscall_scratch boot_scratch;

void arch_syscall_set_kernel_stack(uint64_t stack_top)
{
	/*
	 * Called from sched switch_to() on every context switch.  The
	 * scratch slot is per-CPU, so on single-CPU it always describes
	 * the currently-running task.
	 */
	boot_scratch.kernel_rsp = stack_top;
}

void arch_syscall_set_current_nr(int64_t nr) { boot_scratch.current_nr = nr; }

int64_t arch_syscall_current_nr(void) { return boot_scratch.current_nr; }

void arch_syscall_init(void)
{
	uint64_t star;
	uint64_t fmask;

	/*
	 * SYSRET derives user CS/SS by adding 16/8 to STAR's user field.
	 * With JNU's GDT layout that field must be the kernel data selector.
	 */
	star =
	    ((uint64_t)GDT_KERNEL_DS << 48) | ((uint64_t)GDT_KERNEL_CS << 32);
	fmask = RFLAGS_TF | RFLAGS_IF | RFLAGS_DF | RFLAGS_IOPL | RFLAGS_NT |
		RFLAGS_AC;

	wrmsr(MSR_STAR, star);
	wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
	wrmsr(MSR_FMASK, fmask);
	wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);

	/*
	 * Install the per-CPU syscall scratch into MSR_KERNEL_GS_BASE.
	 * SWAPGS in syscall_entry.S exposes this struct as gs:[0]/gs:[8],
	 * eliminating the previous single-global user-RSP slot that was
	 * hostile to SMP and to any future reentrant kernel work.
	 */
	wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)(uintptr_t)&boot_scratch);

	pr_info("syscall: syscall/sysret MSRs installed\n");
}

void arch_syscall_install_user_gs(void)
{
	wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)(uintptr_t)&boot_scratch);
	wrmsr(MSR_GS_BASE, 0);
}
