/*
 * kernel/syscall/sys_fs_write.c — Filesystem mutation syscalls.
 *
 * Implements v0.0.3.1 path-based write-support syscalls on top of VFS.
 * User paths are copied once into kernel buffers before any filesystem
 * operation so MINIX never receives userspace pointers.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/fd.h>
#include <jnu/process.h>
#include <jnu/sched.h>
#include <jnu/syscall.h>
#include <jnu/vfs.h>

int64_t sys_creat(const char *upath, int mode)
{
	(void)mode;
	return sys_open(upath, 0100 | 01000);
}

int64_t sys_unlink(const char *upath)
{
	char path[JNU_PATH_MAX];
	int err;

	err = syscall_copy_path(path, upath);
	if (err)
		return err;
	return vfs_unlink(path);
}

int64_t sys_mkdir(const char *upath, int mode)
{
	char path[JNU_PATH_MAX];
	int err;

	err = syscall_copy_path(path, upath);
	if (err)
		return err;
	return vfs_mkdir(path, (uint16_t)mode);
}

int64_t sys_rmdir(const char *upath)
{
	char path[JNU_PATH_MAX];
	int err;

	err = syscall_copy_path(path, upath);
	if (err)
		return err;
	return vfs_rmdir(path);
}

int64_t sys_rename(const char *uold, const char *unew)
{
	char old_path[JNU_PATH_MAX];
	char new_path[JNU_PATH_MAX];
	int err;

	err = syscall_copy_path(old_path, uold);
	if (err)
		return err;
	err = syscall_copy_path(new_path, unew);
	if (err)
		return err;
	return vfs_rename(old_path, new_path);
}

int64_t sys_fsync(int fd)
{
	struct task *task = sched_current();
	struct file *file;

	if (!task || !task->process)
		return -EINVAL;
	file = fd_get(&task->process->fds, fd);
	if (!file || file->type != JNU_FILE_VFS)
		return -EINVAL;
	return vfs_fsync(file->u.vfs);
}

int64_t sys_ftruncate(int fd, int64_t length)
{
	struct task *task = sched_current();
	struct file *file;

	if (length < 0)
		return -EINVAL;
	if (!task || !task->process)
		return -EINVAL;
	file = fd_get(&task->process->fds, fd);
	if (!file || file->type != JNU_FILE_VFS)
		return -EINVAL;
	return vfs_truncate(file->u.vfs, (uint64_t)length);
}
