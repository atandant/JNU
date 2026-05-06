/*
 * kernel/syscall/sys_read.c - read syscall.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/chardev.h>
#include <jnu/errno.h>
#include <jnu/fd.h>
#include <jnu/process.h>
#include <jnu/sched.h>
#include <jnu/syscall.h>
#include <jnu/usercopy.h>
#include <jnu/mutex.h>

#define READ_CHUNK 256
#define JNU_O_ACCMODE 03
#define JNU_O_WRONLY 01

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

/*
 * Drain a char_device into userspace.  Read into a kernel bounce
 * buffer first, then copy_to_user(), so the driver never sees the
 * user pointer and the SMAP/usercopy contract from §2.3/§2.8 holds.
 *
 * Char devices are non-seekable and `ubuf` may be only partially
 * mapped; we do not advance file->offset and we return whatever the
 * driver had ready (short read is normal for a tty-style device).
 */
static int64_t chardev_read_to_user(struct char_device *cdev, void *ubuf,
				    size_t len)
{
	char buf[READ_CHUNK];
	size_t done = 0;

	while (done < len) {
		size_t chunk = len - done;
		ssize_t n;
		int err;

		if (chunk > sizeof(buf)) {
			chunk = sizeof(buf);
		}

		n = cdev->ops->read(cdev, buf, chunk);
		if (n < 0) {
			return done ? (int64_t)done : n;
		}
		if (n == 0) {
			break;
		}

		err = copy_to_user((uint8_t *)ubuf + done, buf, (size_t)n);
		if (err) {
			return done ? (int64_t)done : err;
		}

		done += (size_t)n;
		/*
		 * Char devices are stream-like.  A short read from the
		 * driver means "no more bytes ready right now"; do not
		 * spin trying to fill `len`.
		 */
		if ((size_t)n < chunk) {
			break;
		}
	}

	return (int64_t)done;
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

	if (file->type == JNU_FILE_CHARDEV) {
		if ((file->flags & JNU_O_ACCMODE) == JNU_O_WRONLY) {
			return -EACCES;
		}
		if (!file->u.chardev || !file->u.chardev->ops ||
		    !file->u.chardev->ops->read) {
			return -EINVAL;
		}
		mutex_lock(&file->lock);
		int64_t ret = chardev_read_to_user(file->u.chardev, ubuf, len);
		mutex_unlock(&file->lock);
		return ret;
	}
	if ((file->flags & JNU_O_ACCMODE) == JNU_O_WRONLY) {
		return -EACCES;
	}

	mutex_lock(&file->lock);
	while (done < len) {
		size_t chunk = len - done;
		ssize_t n;
		int err;

		if (chunk > sizeof(buf)) {
			chunk = sizeof(buf);
		}

		n = file_read_at(file, file->offset, buf, chunk);
		if (n < 0) {
			mutex_unlock(&file->lock);
			return done ? (int64_t)done : n;
		}
		if (n == 0) {
			break;
		}

		err = copy_to_user((uint8_t *)ubuf + done, buf, (size_t)n);
		if (err) {
			mutex_unlock(&file->lock);
			return done ? (int64_t)done : err;
		}

		file->offset += (uint64_t)n;
		done += (size_t)n;
		if ((size_t)n < chunk) {
			break;
		}
	}
	mutex_unlock(&file->lock);

	return (int64_t)done;
}
