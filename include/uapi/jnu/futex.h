/*
 * include/uapi/jnu/futex.h — Userspace futex ABI.
 *
 * Operation codes and flag bits for the futex(2) syscall, matching the
 * Linux x86_64 ABI so that an unmodified musl libc works against JNU.
 * Only a subset of operations is implemented by the kernel (see
 * kernel/syscall/sys_futex.c); the rest return -ENOSYS.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

/* Base operations (low byte of the op argument). */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_FD 2
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP 5
#define FUTEX_LOCK_PI 6
#define FUTEX_UNLOCK_PI 7
#define FUTEX_TRYLOCK_PI 8
#define FUTEX_WAIT_BITSET 9

/*
 * Flag bits OR'd into the op argument. JNU has a single address space
 * per thread group, so PRIVATE vs shared makes no difference and the
 * flag is simply masked off. CLOCK_REALTIME selects the clock for a
 * timed wait; JNU has one monotonic-ish time source so it is ignored.
 */
#define FUTEX_PRIVATE 128
#define FUTEX_CLOCK_REALTIME 256

/* Mask of flag bits, so the base operation can be extracted. */
#define FUTEX_CMD_MASK (~(FUTEX_PRIVATE | FUTEX_CLOCK_REALTIME))
