/*
 * kernel/syscall/common.c - Shared syscall validation helpers.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/syscall.h>
#include <jnu/usercopy.h>

int syscall_copy_path(char *dst, const char *upath)
{
	return copy_string_from_user(dst, upath, JNU_PATH_MAX);
}
