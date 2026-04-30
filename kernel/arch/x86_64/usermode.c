/*
 * kernel/arch/x86_64/usermode.c - Final ring-3 transition hook.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/gdt.h>
#include <jnu/types.h>
#include <jnu/usermode.h>
#include <jnu/arch_syscall.h>

int usermode_enter(uint64_t entry, uint64_t stack)
{
	if (!entry || !stack) {
		return -EINVAL;
	}

	arch_syscall_install_user_gs();

	/*
	 * iretq pops RIP, CS, RFLAGS, RSP, SS but does NOT touch the
	 * general-purpose registers. Whatever the kernel left in
	 * rax/rbx/rcx/rdx/rsi/rdi/rbp/r8..r15 would be visible to
	 * ring 3 — which can include kernel pointers, stack canaries,
	 * or arbitrary heap addresses. Zero them all immediately
	 * before iretq so userspace starts with a clean register
	 * file. The build of the iretq frame happens before the wipe;
	 * after the wipe, the only live values are RSP (kernel stack
	 * holding the iretq frame) and the temporaries the asm
	 * itself touches.
	 *
	 * We use a memory operand for entry/stack so we are free to
	 * scrub every register in the GP set; reading them through
	 * the iretq stack frame avoids needing them in registers
	 * after the wipe.
	 */
	__asm__ __volatile__(
		"cli\n\t"
		"mov %[uds], %%ax\n\t"
		"mov %%ax, %%ds\n\t"
		"mov %%ax, %%es\n\t"
		"mov %%ax, %%fs\n\t"
		"mov %%ax, %%gs\n\t"
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
		: [uds] "i"(GDT_USER_DS | 3),
		  [ucs] "i"(GDT_USER_CS | 3),
		  [rsp] "r"(stack),
		  [rip] "r"(entry)
		: "rax", "memory");

	return -EFAULT;
}
