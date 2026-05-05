/*
 * kernel/syscall/sys_open.c - open syscall.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/fd.h>
#include <jnu/initramfs.h>
#include <jnu/kbd.h>
#include <jnu/kmalloc.h>
#include <jnu/process.h>
#include <jnu/sched.h>
#include <jnu/string.h>
#include <jnu/syscall.h>
#include <jnu/vfs.h>

#define JNU_O_ACCMODE 03
#define JNU_O_RDONLY 00
#define JNU_O_WRONLY 01
#define JNU_O_RDWR 02
#define JNU_O_CREAT 0100
#define JNU_O_TRUNC 01000
#define JNU_O_APPEND 02000
#define JNU_O_LARGEFILE 0100000

/*
 * Resolve a synthetic /dev/ path to a registered char_device.
 * Returns NULL when the path is not a known device node.  This is the
 * narrowest entry point that lets userspace read PS/2 keyboard input
 * (open("/dev/kbd") + read()) without granting char drivers direct
 * access to user pointers.  Future device nodes register here.
 */
static struct char_device *resolve_dev_chardev(const char *path)
{
	if (strcmp(path, "/dev/kbd") == 0) {
		return kbd_get_chardev();
	}
	return NULL;
}

int64_t sys_open(const char *upath, int flags)
{
	char path[JNU_PATH_MAX];
	struct task *task;
	struct file *file;
	struct char_device *cdev;
	int err;

	if ((flags & ~(JNU_O_ACCMODE | JNU_O_CREAT | JNU_O_TRUNC |
		       JNU_O_APPEND | JNU_O_LARGEFILE)) != 0) {
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
	/*
	 * The slot installed by fd_alloc() owns this initial reference;
	 * fd_alloc itself does NOT bump refcount. file_put on the
	 * fail-path or sys_close drops it.
	 */
	file->refcount = 1;
	file->flags = (uint32_t)flags;

	cdev = resolve_dev_chardev(path);
	if (cdev) {
		file->type = JNU_FILE_CHARDEV;
		file->u.chardev = cdev;
		goto alloc_fd;
	}

	err = initramfs_lookup(path, &file->u.initramfs);
	if (!err) {
		file->type = JNU_FILE_INITRAMFS;
		goto alloc_fd;
	}

	err = vfs_open(path, &file->u.vfs);
	if (err == -ENOENT && (flags & JNU_O_CREAT) != 0)
		err = vfs_create(path, 0666, &file->u.vfs);
	if (err) {
		goto fail_file;
	}
	file->type = JNU_FILE_VFS;
	if ((flags & JNU_O_TRUNC) != 0) {
		err = vfs_truncate(file->u.vfs, 0);
		if (err)
			goto fail_file;
	}
	if ((flags & JNU_O_APPEND) != 0)
		file->offset = file->u.vfs->size;

alloc_fd:
	err = fd_alloc(&task->process->fds, file);
	if (err < 0) {
		goto fail_file;
	}
	return err;

fail_file:
	/*
	 * file_put drives the type-specific teardown (vfs_close, etc.)
	 * via the same destroy path that sys_close eventually uses.
	 */
	file_put(file);
	return err;
}
