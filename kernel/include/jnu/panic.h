/*
 * include/jnu/panic.h — Kernel panic entry point.
 *
 * Phase 1: minimal panic. Format a headline, write it to all klog
 * backends in panic mode, then `cli; hlt; jmp $`. Register dump and
 * backtrace come in Phase 2.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/compiler.h>

struct cpu_state;	/* defined in <jnu/idt.h> */

__noreturn void panic(const char *fmt, ...) __printf(1, 2);

/*
 * Panic with a saved CPU state. Used by exception handlers so the
 * forensic dump (§13) shows the faulting register file, faulting RIP,
 * CR2, and a frame-pointer backtrace anchored at saved RBP.
 */
__noreturn void panic_with_state(struct cpu_state *st);
