/*
 * kernel/syscall/dispatch.c - Native syscall dispatcher.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/syscall.h>
#include <jnu/syscall_nr.h>

int64_t syscall_dispatch(const struct syscall_args *args)
{
	if (!args) {
		return -EINVAL;
	}

	switch (args->nr) {
	case JNU_SYS_exit:
		return sys_exit((int)args->arg0);
	case JNU_SYS_write:
		return sys_write((int)args->arg0, (const void *)args->arg1,
				 (size_t)args->arg2);
	case JNU_SYS_read:
		return sys_read((int)args->arg0, (void *)args->arg1,
				(size_t)args->arg2);
	case JNU_SYS_open:
		return sys_open((const char *)args->arg0, (int)args->arg1);
	case JNU_SYS_close:
		return sys_close((int)args->arg0);
	case JNU_SYS_lseek:
		return sys_lseek((int)args->arg0, (int64_t)args->arg1,
				 (int)args->arg2);
	case JNU_SYS_getpid:
		return sys_getpid();
	case JNU_SYS_yield:
		return sys_yield();
	case JNU_SYS_fstat:
		return sys_fstat((int)args->arg0, (void *)args->arg1);
	case JNU_SYS_spawn:
		return sys_spawn((const char *)args->arg0,
				 (char *const *)args->arg1);
	case JNU_SYS_waitpid:
		return sys_waitpid((int)args->arg0, (int *)args->arg1);
	default:
		return -ENOSYS;
	}
}

int syscall_selftest(void)
{
	struct syscall_args args = {.nr = JNU_SYS_MAX + 1};
	int64_t ret = syscall_dispatch(&args);

	return ret == -ENOSYS ? 0 : -EINVAL;
}
