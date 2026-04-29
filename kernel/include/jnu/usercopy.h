/*
 * include/jnu/usercopy.h - Userspace pointer validation and copying.
 *
 * Syscall handlers must not dereference userspace pointers directly.
 * These helpers centralize range checks, PTE_USER permission checks,
 * and the eventual SMAP stac/clac window.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>

#define USER_TOP	0x0000800000000000ull

bool user_range_ok(const void *uaddr, size_t len);
int user_range_mapped(const void *uaddr, size_t len, bool write);
int copy_from_user(void *dst, const void *usrc, size_t len);
int copy_to_user(void *udst, const void *src, size_t len);
int copy_string_from_user(char *dst, const char *usrc, size_t max);
int usercopy_selftest(void);
