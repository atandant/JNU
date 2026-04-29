/*
 * kernel/syscall/sys_open.c - open syscall.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/fd.h>
#include <jnu/initramfs.h>
#include <jnu/kmalloc.h>
#include <jnu/process.h>
#include <jnu/sched.h>
#include <jnu/syscall.h>
#include <jnu/vfs.h>

int64_t sys_open(const char *upath, int flags)
{
	char path[JNU_PATH_MAX];
	struct task *task;
	struct file *file;
	int err;

	if (flags != 0) {
		return -EINVAL;
	}

	err = syscall_copy_path(path, upath);
	if (err) {
		return err;
	}

	task = sched_current();
	if (!task || !task->process) {
		return -EINVAL;
	}

	file = kzalloc(sizeof(*file));
	if (!file) {
		return -ENOMEM;
	}

	err = initramfs_lookup(path, &file->u.initramfs);
	if (!err) {
		file->type = JNU_FILE_INITRAMFS;
		goto alloc_fd;
	}

	err = vfs_open(path, &file->u.vfs);
	if (err) {
		goto fail_file;
	}
	file->type = JNU_FILE_VFS;

alloc_fd:
	err = fd_alloc(&task->process->fds, file);
	if (err < 0) {
		goto fail_backing;
	}
	return err;

fail_backing:
	if (file->type == JNU_FILE_VFS) {
		vfs_close(file->u.vfs);
	}
fail_file:
	kfree(file);
	return err;
}
