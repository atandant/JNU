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
#include <jnu/mutex.h>

#define JNU_SEEK_SET 0
#define JNU_SEEK_CUR 1
#define JNU_SEEK_END 2

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

	/* Char devices (e.g. /dev/kbd) are streams. */
	if (file->type == JNU_FILE_CHARDEV) {
		return -ESPIPE;
	}

	mutex_lock(&file->lock);

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
		mutex_unlock(&file->lock);
		return -EINVAL;
	}
	/*
	 * Reject signed overflow, negative results, and offsets past EOF.
	 * The previous code only rejected `next >= INT64_MAX`, which is
	 * functionally the same as the overflow check and let userspace
	 * pin file->offset at any 63-bit value. Since v0.0.2 filesystems
	 * are read-only, capping at file_size() is both safe and what the
	 * read path expects (minix_read short-circuits at offset >= size,
	 * but only after a uint64_t + size_t addition that overflows for
	 * sufficiently large offsets — see kernel/fs/minix.c).
	 */
	if (__builtin_add_overflow(base, off, &next) || next < 0 ||
	    next > (int64_t)file_size(file)) {
		mutex_unlock(&file->lock);
		return -EINVAL;
	}

	file->offset = (uint64_t)next;
	mutex_unlock(&file->lock);
	return next;
}
