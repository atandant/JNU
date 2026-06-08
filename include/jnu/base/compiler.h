/*
 * include/jnu/base/compiler.h — Compiler attribute and intrinsic shims.
 *
 * Centralizes attribute spellings so the rest of the kernel reads cleanly
 * regardless of which clang version we are compiled with. Adds branch-
 * prediction hints and barrier helpers used across subsystems.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define __noreturn __attribute__((noreturn))
#define __used __attribute__((used))
#define __unused __attribute__((unused))
#define __section(s) __attribute__((section(s)))
#define __weak __attribute__((weak))
#define __printf(a, b) __attribute__((format(printf, a, b)))
#define __must_check __attribute__((warn_unused_result))

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define barrier() __asm__ __volatile__("" ::: "memory")

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
