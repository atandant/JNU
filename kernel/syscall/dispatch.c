/*
 * kernel/syscall/dispatch.c - Native syscall dispatcher.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/arch_syscall.h>
#include <jnu/syscall.h>
#include <jnu/syscall_nr.h>

int64_t syscall_dispatch(const struct syscall_args *args)
{
	int64_t ret;

	if (!args) {
		return -EINVAL;
	}

	arch_syscall_set_current_nr((int64_t)args->nr);

	switch (args->nr) {
	case JNU_SYS_exit:
		ret = sys_exit((int)args->arg0);
		break;
	case JNU_SYS_write:
		ret = sys_write((int)args->arg0, (const void *)args->arg1,
				(size_t)args->arg2);
		break;
	case JNU_SYS_read:
		ret = sys_read((int)args->arg0, (void *)args->arg1,
			       (size_t)args->arg2);
		break;
	case JNU_SYS_open:
		ret = sys_open((const char *)args->arg0, (int)args->arg1);
		break;
	case JNU_SYS_close:
		ret = sys_close((int)args->arg0);
		break;
	case JNU_SYS_lseek:
		ret = sys_lseek((int)args->arg0, (int64_t)args->arg1,
				(int)args->arg2);
		break;
	case JNU_SYS_getpid:
		ret = sys_getpid();
		break;
	case JNU_SYS_yield:
		ret = sys_yield();
		break;
	case JNU_SYS_fstat:
		ret = sys_fstat((int)args->arg0, (void *)args->arg1);
		break;
	case JNU_SYS_spawn:
		ret = -ENOSYS;
		break;
	case JNU_SYS_waitpid:
		ret = sys_waitpid((int)args->arg0, (int *)args->arg1);
		break;
	case JNU_SYS_fork:
		ret = sys_fork(args);
		break;
	case JNU_SYS_execve:
		ret = sys_execve(args);
		break;
	default:
		ret = -ENOSYS;
		break;
	}

	arch_syscall_set_current_nr(-1);
	return ret;
}

int syscall_selftest(void)
{
	struct syscall_args args = {.nr = JNU_SYS_MAX + 1};
	int64_t ret = syscall_dispatch(&args);

	return ret == -ENOSYS ? 0 : -EINVAL;
}
