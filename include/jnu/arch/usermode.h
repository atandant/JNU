/*
 * include/jnu/arch/usermode.h - Architecture userspace entry.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/base/types.h>

struct syscall_frame;

int usermode_enter(uint64_t entry, uint64_t stack);
int usermode_enter_fork_frame(const struct syscall_frame *frame);
