/*
 * include/jnu/mman.h — Linux-compatible mmap/mprotect constants.
 *
 * v0.0.3 §2.3: flag values are intentionally Linux-compatible so that
 * musl's static startup can pass the same constants it would pass on
 * Linux.  Only MAP_PRIVATE | MAP_ANONYMOUS is supported in v0.0.3.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

/* Protection flags — passed to mmap() and mprotect(). */
#define PROT_NONE  0x00
#define PROT_READ  0x01
#define PROT_WRITE 0x02
#define PROT_EXEC  0x04

/* Map flags — passed to mmap(). */
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

/*
 * Top of the mmap hint region.  mmap() without MAP_FIXED walks
 * top-down from this address to find the first gap large enough
 * for the requested size.
 */
#define MMAP_BASE 0x0000555500000000ull

/*
 * Boundary of the user-accessible virtual address space.  Any VA at
 * or above this value is kernel territory.
 */
#ifndef USER_TOP
#define USER_TOP 0x0000800000000000ull
#endif
