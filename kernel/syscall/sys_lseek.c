/*
 * kernel/syscall/sys_lseek.c - lseek syscall.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/fd.h>
#include <jnu/process.h>
#include <jnu/sched.h>
#include <jnu/syscall.h>

#define JNU_SEEK_SET	0
#define JNU_SEEK_CUR	1
#define JNU_SEEK_END	2

static uint64_t file_size(const struct file *file)
{
	if (file->type == JNU_FILE_INITRAMFS) {
		return file->u.initramfs.size;
	}
	if (file->type == JNU_FILE_VFS) {
		return file->u.vfs->size;
	}
	return 0;
}

int64_t sys_lseek(int fd, int64_t off, int whence)
{
	struct task *task = sched_current();
	struct file *file;
	int64_t base;
	int64_t next;

	if (!task || !task->process) {
		return -EINVAL;
	}

	file = fd_get(&task->process->fds, fd);
	if (!file) {
		return -EINVAL;
	}

	switch (whence) {
	case JNU_SEEK_SET:
		base = 0;
		break;
	case JNU_SEEK_CUR:
		base = (int64_t)file->offset;
		break;
	case JNU_SEEK_END:
		base = (int64_t)file_size(file);
		break;
	default:
		return -EINVAL;
	}

	next = base + off;
	if (next < 0 || (off > 0 && next < base) ||
	    (off < 0 && next > base)) {
		return -EINVAL;
	}

	file->offset = (uint64_t)next;
	return next;
}
