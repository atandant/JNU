/*
 * kernel/syscall/sys_close.c - close syscall.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/fd.h>
#include <jnu/process.h>
#include <jnu/sched.h>
#include <jnu/syscall.h>

int64_t sys_close(int fd)
{
	struct task *task = sched_current();
	struct file *file;

	if (!task || !task->process) {
		return -EINVAL;
	}

	file = fd_close(&task->process->fds, fd);
	if (!file) {
		return -EINVAL;
	}

	file_put(file);
	return 0;
}
