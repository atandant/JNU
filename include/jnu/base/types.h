/*
 * include/jnu/base/types.h — Primitive integer and address typedefs.
 *
 * Freestanding type definitions. We do not pull <stdint.h> from a host
 * libc; clang's freestanding builtin headers provide it, but we re-export
 * a curated subset under JNU's spelling conventions.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef signed short int16_t;
typedef unsigned short uint16_t;
typedef signed int int32_t;
typedef unsigned int uint32_t;
typedef signed long long int64_t;
typedef unsigned long long uint64_t;

typedef unsigned long size_t;
typedef signed long ssize_t;
typedef unsigned long uintptr_t;
typedef signed long intptr_t;
typedef signed long ptrdiff_t;

typedef uint64_t paddr_t; /* physical address */
typedef uint64_t vaddr_t; /* virtual address */

typedef _Bool bool;
#define true 1
#define false 0

#define NULL ((void *)0)

_Static_assert(sizeof(uint8_t) == 1, "uint8_t size");
_Static_assert(sizeof(uint16_t) == 2, "uint16_t size");
_Static_assert(sizeof(uint32_t) == 4, "uint32_t size");
_Static_assert(sizeof(uint64_t) == 8, "uint64_t size");
_Static_assert(sizeof(void *) == 8, "x86_64 pointer size");
