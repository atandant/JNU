/*
 * kernel/syscall/sys_spawn.c - spawn syscall.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/klog.h>
#include <jnu/process.h>
#include <jnu/string.h>
#include <jnu/syscall.h>

int64_t sys_spawn(const char *upath, char *const *uargv)
{
	char path[JNU_PATH_MAX];
	int pid;
	int err;

	(void)uargv;

	err = syscall_copy_path(path, upath);
	if (err) {
		return err;
	}

	err = process_spawn(path, uargv, &pid);
	if (err) {
		return err;
	}
	return pid;
}
