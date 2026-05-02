/*
 * include/jnu/arch_syscall.h - x86_64 syscall CPU setup.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>

void arch_syscall_init(void);
void arch_syscall_set_kernel_stack(uint64_t stack_top);
void arch_syscall_set_current_nr(int64_t nr);
int64_t arch_syscall_current_nr(void);

/*
 * Reinstall MSR_KERNEL_GS_BASE (and clear MSR_GS_BASE) immediately
 * before iretq-ing to userspace.
 *
 * Why this exists: usermode_enter loads the user data selector into
 * the GS segment register, which on x86_64 zeroes IA32_GS_BASE as a
 * side effect.  More importantly, IA32_KERNEL_GS_BASE may have been
 * clobbered by an earlier swapgs that the original task never
 * unwound — when a task is preempted *inside* its syscall handler
 * (post-swapgs), KERNEL_GS_BASE holds 0 (the previous user GS), and
 * if a fresh task is then scheduled and reaches usermode_enter, it
 * would iretq with KERNEL_GS_BASE = 0.  The new task's very first
 * syscall would then SWAPGS 0 into GS_BASE, and `mov [gs:0], rsp`
 * would write to NULL.
 *
 * Calling this right before iretq guarantees the per-CPU scratch is
 * reachable via SWAPGS for the next syscall, regardless of how messy
 * the GS state was during kernel-side preparation.
 */
void arch_syscall_install_user_gs(void);
