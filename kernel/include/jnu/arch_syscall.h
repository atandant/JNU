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
