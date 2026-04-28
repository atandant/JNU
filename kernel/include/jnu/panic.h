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

__noreturn void panic(const char *fmt, ...) __printf(1, 2);
