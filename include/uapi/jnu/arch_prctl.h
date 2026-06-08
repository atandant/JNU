/*
 * include/uapi/jnu/arch_prctl.h — arch_prctl request codes (x86_64).
 *
 * Linux-compatible values for musl TLS setup via ARCH_SET_FS.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004
