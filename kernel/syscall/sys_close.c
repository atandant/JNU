/*
 * kernel/syscall/sys_close.c - close syscall.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/kernel/process.h>
#include <jnu/kernel/sched.h>
#include <jnu/user/fd.h>
#include <jnu/user/syscall.h>
#include <uapi/jnu/errno.h>

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
