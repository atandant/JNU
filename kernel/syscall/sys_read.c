/*
 * kernel/syscall/sys_read.c - read syscall.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/fd.h>
#include <jnu/process.h>
#include <jnu/sched.h>
#include <jnu/syscall.h>
#include <jnu/usercopy.h>

#define READ_CHUNK	256

static ssize_t file_read_at(struct file *file, uint64_t off, void *buf,
			    size_t len)
{
	if (file->type == JNU_FILE_INITRAMFS) {
		return initramfs_read_at(&file->u.initramfs, off, buf, len);
	}
	if (file->type == JNU_FILE_VFS) {
		return vfs_read(file->u.vfs, off, len, buf);
	}
	return -EINVAL;
}

int64_t sys_read(int fd, void *ubuf, size_t len)
{
	struct task *task = sched_current();
	struct file *file;
	char buf[READ_CHUNK];
	size_t done = 0;

	if (!task || !task->process) {
		return -EINVAL;
	}

	file = fd_get(&task->process->fds, fd);
	if (!file) {
		return -EINVAL;
	}

	while (done < len) {
		size_t chunk = len - done;
		ssize_t n;
		int err;

		if (chunk > sizeof(buf)) {
			chunk = sizeof(buf);
		}

		n = file_read_at(file, file->offset, buf, chunk);
		if (n < 0) {
			return done ? (int64_t)done : n;
		}
		if (n == 0) {
			break;
		}

		err = copy_to_user((uint8_t *)ubuf + done, buf, (size_t)n);
		if (err) {
			return err;
		}

		file->offset += (uint64_t)n;
		done += (size_t)n;
		if ((size_t)n < chunk) {
			break;
		}
	}

	return (int64_t)done;
}
