/*
 * include/jnu/usermode.h - Architecture userspace entry.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>

int usermode_enter(uint64_t entry, uint64_t stack);
