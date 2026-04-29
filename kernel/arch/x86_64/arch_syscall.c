/*
 * kernel/arch/x86_64/arch_syscall.c - syscall/sysret MSR setup.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/arch_syscall.h>
#include <jnu/cpu.h>
#include <jnu/gdt.h>
#include <jnu/klog.h>
#include <jnu/types.h>

#define MSR_STAR	0xC0000081
#define MSR_LSTAR	0xC0000082
#define MSR_FMASK	0xC0000084

#define EFER_SCE	(1ull << 0)

#define RFLAGS_TF	(1ull << 8)
#define RFLAGS_IF	(1ull << 9)
#define RFLAGS_DF	(1ull << 10)
#define RFLAGS_IOPL	(3ull << 12)
#define RFLAGS_NT	(1ull << 14)
#define RFLAGS_AC	(1ull << 18)

extern void syscall_entry(void);
extern uint64_t syscall_kernel_stack_top;

void arch_syscall_set_kernel_stack(uint64_t stack_top)
{
	syscall_kernel_stack_top = stack_top;
}

void arch_syscall_init(void)
{
	uint64_t star;
	uint64_t fmask;

	/*
	 * SYSRET derives user CS/SS by adding 16/8 to STAR's user field.
	 * With JNU's GDT layout that field must be the kernel data selector.
	 */
	star = ((uint64_t)GDT_KERNEL_DS << 48) |
	       ((uint64_t)GDT_KERNEL_CS << 32);
	fmask = RFLAGS_TF | RFLAGS_IF | RFLAGS_DF | RFLAGS_IOPL |
		RFLAGS_NT | RFLAGS_AC;

	wrmsr(MSR_STAR, star);
	wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
	wrmsr(MSR_FMASK, fmask);
	wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);

	pr_info("syscall: syscall/sysret MSRs installed\n");
}
