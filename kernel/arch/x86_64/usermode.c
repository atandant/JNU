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

int usermode_enter(uint64_t entry, uint64_t stack)
{
	if (!entry || !stack) {
		return -EINVAL;
	}

	__asm__ __volatile__(
		"cli\n\t"
		"mov %[uds], %%ax\n\t"
		"mov %%ax, %%ds\n\t"
		"mov %%ax, %%es\n\t"
		"pushq %[uds]\n\t"
		"pushq %[rsp]\n\t"
		"pushfq\n\t"
		"orq $0x200, (%%rsp)\n\t"
		"pushq %[ucs]\n\t"
		"pushq %[rip]\n\t"
		"iretq\n\t"
		:
		: [uds] "i"(GDT_USER_DS | 3),
		  [ucs] "i"(GDT_USER_CS | 3),
		  [rsp] "r"(stack),
		  [rip] "r"(entry)
		: "rax", "memory");

	return -EFAULT;
}
