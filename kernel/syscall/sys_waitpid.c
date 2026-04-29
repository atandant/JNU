/*
 * kernel/syscall/sys_waitpid.c - waitpid syscall.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/process.h>
#include <jnu/syscall.h>
#include <jnu/usercopy.h>

int64_t sys_waitpid(int pid, int *ustatus)
{
	int status = 0;
	int err;

	err = process_wait(pid, ustatus ? &status : NULL);
	if (err < 0) {
		return err;
	}

	if (ustatus) {
		int copy_err = copy_to_user(ustatus, &status, sizeof(status));
		if (copy_err) {
			return copy_err;
		}
	}

	return err;
}
