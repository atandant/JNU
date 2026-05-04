/*
 * kernel/arch/x86_64/usermode.c - Final ring-3 transition hook.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/arch_syscall.h>
#include <jnu/cpu.h>
#include <jnu/errno.h>
#include <jnu/gdt.h>
#include <jnu/sched.h>
#include <jnu/syscall.h>
#include <jnu/types.h>
#include <jnu/usermode.h>

int usermode_enter(uint64_t entry, uint64_t stack)
{
	struct task *t = sched_current();
	uint32_t user_ds = GDT_USER_DS | 3;

	if (!entry || !stack) {
		return -EINVAL;
	}

	__asm__ __volatile__(
	    "mov %0, %%ds\n\t"
	    "mov %0, %%es\n\t"
	    "mov %0, %%fs\n\t"
	    "mov %0, %%gs\n\t"
	    :
	    : "r"(user_ds)
	    : "memory");

	arch_syscall_install_user_gs();
	if (t) {
		wrmsr(MSR_FS_BASE, t->fs_base);
		wrmsr(MSR_GS_BASE, t->gs_base);
	}

	__asm__ __volatile__(
	    "cli\n\t"
	    "pushq %[uds]\n\t"
	    "pushq %[rsp]\n\t"
	    "pushfq\n\t"
	    "orq $0x200, (%%rsp)\n\t"
	    "pushq %[ucs]\n\t"
	    "pushq %[rip]\n\t"
	    "xor %%rax, %%rax\n\t"
	    "xor %%rbx, %%rbx\n\t"
	    "xor %%rcx, %%rcx\n\t"
	    "xor %%rdx, %%rdx\n\t"
	    "xor %%rsi, %%rsi\n\t"
	    "xor %%rdi, %%rdi\n\t"
	    "xor %%rbp, %%rbp\n\t"
	    "xor %%r8, %%r8\n\t"
	    "xor %%r9, %%r9\n\t"
	    "xor %%r10, %%r10\n\t"
	    "xor %%r11, %%r11\n\t"
	    "xor %%r12, %%r12\n\t"
	    "xor %%r13, %%r13\n\t"
	    "xor %%r14, %%r14\n\t"
	    "xor %%r15, %%r15\n\t"
	    "iretq\n\t"
	    :
	    : [uds] "i"(GDT_USER_DS | 3), [ucs] "i"(GDT_USER_CS | 3),
	      [rsp] "r"(stack), [rip] "r"(entry)
	    : "rax", "memory");

	return -EFAULT;
}

int usermode_enter_fork_frame(const struct syscall_frame *frame)
{
	if (!frame || !frame->user.rip || !frame->user.rsp) {
		return -EINVAL;
	}

	arch_syscall_install_user_gs();

	/*
	 * Enter the fork child as if the syscall path had returned from
	 * the parent's saved frame, except rax is forced to 0.  This keeps
	 * the cloned userspace stack and callee-saved registers coherent
	 * for code that continues after fork().
	 */
	__asm__ __volatile__(
	    "cli\n\t"
	    "mov %[uds], %%ax\n\t"
	    "mov %%ax, %%ds\n\t"
	    "mov %%ax, %%es\n\t"
	    "mov %%ax, %%fs\n\t"
	    "mov %%ax, %%gs\n\t"
	    "movq %[frame], %%rax\n\t"
	    "pushq %[uds]\n\t"
	    "pushq 72(%%rax)\n\t"
	    "pushq 56(%%rax)\n\t"
	    "orq $0x202, (%%rsp)\n\t"
	    "pushq %[ucs]\n\t"
	    "pushq 64(%%rax)\n\t"
	    "movq 120(%%rax), %%r15\n\t"
	    "movq 112(%%rax), %%r14\n\t"
	    "movq 104(%%rax), %%r13\n\t"
	    "movq 80(%%rax), %%r12\n\t"
	    "movq 96(%%rax), %%rbp\n\t"
	    "movq 88(%%rax), %%rbx\n\t"
	    "movq 8(%%rax), %%rdi\n\t"
	    "movq 16(%%rax), %%rsi\n\t"
	    "movq 24(%%rax), %%rdx\n\t"
	    "movq 32(%%rax), %%r10\n\t"
	    "movq 40(%%rax), %%r8\n\t"
	    "movq 48(%%rax), %%r9\n\t"
	    "movq 56(%%rax), %%r11\n\t"
	    "movq 64(%%rax), %%rcx\n\t"
	    "xor %%rax, %%rax\n\t"
	    "iretq\n\t"
	    :
	    : [uds] "i"(GDT_USER_DS | 3), [ucs] "i"(GDT_USER_CS | 3),
	      [frame] "r"(frame)
	    : "memory");

	__builtin_unreachable();
}
