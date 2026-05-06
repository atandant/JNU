/*
 * kernel/syscall/sys_write.c - write syscall.
 *
 * fd 1 (stdout) and fd 2 (stderr) are routed through klog_user_write,
 * which line-prefixes every chunk with "user[pid=N]: " so userspace
 * bytes cannot impersonate a kernel-formatted ring-buffer line and
 * cannot poison the panic-tail dump documented in jnuspec.md §2.11.
 * Control bytes (other than '\n' and '\t') are scrubbed in the kernel
 * bounce buffer to keep ANSI escapes off the COM1 backend.
 *
 * We continue to avoid klog_panic_write here: that path is reserved
 * for panic() and must not be reachable from userspace under any
 * circumstance.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/fd.h>
#include <jnu/klog.h>
#include <jnu/process.h>
#include <jnu/sched.h>
#include <jnu/string.h>
#include <jnu/syscall.h>
#include <jnu/usercopy.h>
#include <jnu/vfs.h>
#include <jnu/mutex.h>

#define WRITE_CHUNK 128
#define JNU_O_ACCMODE 03
#define JNU_O_RDONLY 00

/*
 * Strip every byte that could drive a terminal or be confused with a
 * structural kernel-log byte. We keep '\n' (klog_user_write needs it
 * for line splitting) and '\t' (harmless on every backend we ship).
 * Everything else below 0x20, plus DEL (0x7F), becomes '.'.
 */
static void sanitize(char *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)buf[i];

		if (c == '\n' || c == '\t') {
			continue;
		}
		if (c < 0x20 || c == 0x7F) {
			buf[i] = '.';
		}
	}
}

int64_t sys_write(int fd, const void *ubuf, size_t len)
{
	char buf[WRITE_CHUNK + 1];
	struct task *task = sched_current();
	int pid = (task && task->process) ? task->process->pid : 0;
	enum klog_level level;
	size_t done = 0;
	struct file *file;

	if (fd == 1) {
		level = KLOG_INFO;
	} else if (fd == 2) {
		level = KLOG_ERR;
	} else {
		if (!task || !task->process)
			return -EINVAL;
		file = fd_get(&task->process->fds, fd);
		if (!file || file->type != JNU_FILE_VFS)
			return -EINVAL;
		if ((file->flags & JNU_O_ACCMODE) == JNU_O_RDONLY)
			return -EACCES;

		mutex_lock(&file->lock);
		while (done < len) {
			size_t chunk = len - done;
			ssize_t n;
			int err;

			if (chunk > WRITE_CHUNK)
				chunk = WRITE_CHUNK;
			err = copy_from_user(buf, (const uint8_t *)ubuf + done,
					     chunk);
			if (err) {
				mutex_unlock(&file->lock);
				return done ? (int64_t)done : err;
			}
			n = vfs_write(file->u.vfs, file->offset, chunk, buf);
			if (n < 0) {
				mutex_unlock(&file->lock);
				return done ? (int64_t)done : n;
			}
			file->offset += (uint64_t)n;
			done += (size_t)n;
			if ((size_t)n < chunk)
				break;
		}
		mutex_unlock(&file->lock);
		return (int64_t)done;
	}

	while (done < len) {
		size_t chunk = len - done;
		int err;

		if (chunk > WRITE_CHUNK) {
			chunk = WRITE_CHUNK;
		}

		err = copy_from_user(buf, (const uint8_t *)ubuf + done, chunk);
		if (err) {
			return done ? (int64_t)done : err;
		}

		sanitize(buf, chunk);
		klog_user_write(level, pid, buf, chunk);
		done += chunk;
	}

	return (int64_t)done;
}
