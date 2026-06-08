/*
 * kernel/syscall/sys_fstat.c - fstat syscall.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/kernel/process.h>
#include <jnu/kernel/sched.h>
#include <jnu/user/fd.h>
#include <jnu/user/syscall.h>
#include <jnu/user/usercopy.h>
#include <uapi/jnu/errno.h>

#define JNU_DT_REG 1
#define JNU_DT_DIR 2
#define JNU_DT_CHR 3

int64_t sys_fstat(int fd, void *ust)
{
	struct task *task = sched_current();
	struct file *file;
	struct jnu_stat st;

	if (!task || !task->process) {
		return -EINVAL;
	}

	file = fd_get(&task->process->fds, fd);
	if (!file) {
		return -EINVAL;
	}

	if (file->type == JNU_FILE_INITRAMFS) {
		st.ino = 0;
		st.size = file->u.initramfs.size;
		st.mode = file->u.initramfs.mode;
		st.type = (file->u.initramfs.mode & 0040000) ? JNU_DT_DIR
							     : JNU_DT_REG;
	} else if (file->type == JNU_FILE_VFS) {
		st.ino = file->u.vfs->ino;
		st.size = file->u.vfs->size;
		st.mode = file->u.vfs->mode;
		st.type = file->u.vfs->is_dir ? JNU_DT_DIR : JNU_DT_REG;
	} else if (file->type == JNU_FILE_CHARDEV) {
		st.ino = 0;
		st.size = 0;
		st.mode = 0;
		st.type = JNU_DT_CHR;
	} else {
		return -EINVAL;
	}

	return copy_to_user(ust, &st, sizeof(st));
}
