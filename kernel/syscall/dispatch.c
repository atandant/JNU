/*
 * kernel/syscall/dispatch.c - Native syscall dispatcher.
 *
 * v0.0.3 §2.2: sparse dispatch table indexed by Linux x86_64 syscall
 * number.  Every slot that does not correspond to an implemented syscall
 * is NULL; the dispatcher returns -ENOSYS for unhandled numbers.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/arch_syscall.h>
#include <jnu/syscall.h>
#include <jnu/syscall_nr.h>

typedef int64_t (*syscall_handler_t)(const struct syscall_args *args);

/*
 * Thin wrappers that adapt each handler's real prototype to the
 * uniform (const struct syscall_args *) signature used by the table.
 */

static int64_t wrap_read(const struct syscall_args *a)
{
	return sys_read((int)a->arg0, (void *)a->arg1, (size_t)a->arg2);
}

static int64_t wrap_write(const struct syscall_args *a)
{
	return sys_write((int)a->arg0, (const void *)a->arg1,
			 (size_t)a->arg2);
}

static int64_t wrap_open(const struct syscall_args *a)
{
	return sys_open((const char *)a->arg0, (int)a->arg1);
}

static int64_t wrap_close(const struct syscall_args *a)
{
	return sys_close((int)a->arg0);
}

static int64_t wrap_fstat(const struct syscall_args *a)
{
	return sys_fstat((int)a->arg0, (void *)a->arg1);
}

static int64_t wrap_lseek(const struct syscall_args *a)
{
	return sys_lseek((int)a->arg0, (int64_t)a->arg1, (int)a->arg2);
}

static int64_t wrap_sched_yield(const struct syscall_args *a)
{
	(void)a;
	return sys_yield();
}

static int64_t wrap_getpid(const struct syscall_args *a)
{
	(void)a;
	return sys_getpid();
}

static int64_t wrap_fork(const struct syscall_args *a)
{
	return sys_fork(a);
}

static int64_t wrap_execve(const struct syscall_args *a)
{
	return sys_execve(a);
}

static int64_t wrap_exit(const struct syscall_args *a)
{
	return sys_exit((int)a->arg0);
}

static int64_t wrap_wait4(const struct syscall_args *a)
{
	/*
	 * v0.0.3 §2.2: wait4(pid, status, options, rusage).
	 * JNU ignores options and rusage for now; forward to the
	 * existing sys_waitpid(pid, status) implementation.
	 */
	return sys_waitpid((int)a->arg0, (int *)a->arg1);
}

/*
 * Sparse dispatch table.  Index == Linux x86_64 syscall number.
 * NULL entries return -ENOSYS.
 */
static const syscall_handler_t syscall_table[JNU_SYS_MAX + 1] = {
	[JNU_SYS_read]        = wrap_read,
	[JNU_SYS_write]       = wrap_write,
	[JNU_SYS_open]        = wrap_open,
	[JNU_SYS_close]       = wrap_close,
	[JNU_SYS_fstat]       = wrap_fstat,
	[JNU_SYS_lseek]       = wrap_lseek,
	[JNU_SYS_sched_yield] = wrap_sched_yield,
	[JNU_SYS_getpid]      = wrap_getpid,
	[JNU_SYS_fork]        = wrap_fork,
	[JNU_SYS_execve]      = wrap_execve,
	[JNU_SYS_exit]        = wrap_exit,
	[JNU_SYS_wait4]       = wrap_wait4,
};

int64_t syscall_dispatch(const struct syscall_args *args)
{
	if (!args) {
		return -EINVAL;
	}

	arch_syscall_set_current_nr((int64_t)args->nr);

	int64_t ret;

	if (args->nr <= JNU_SYS_MAX && syscall_table[args->nr]) {
		ret = syscall_table[args->nr](args);
	} else {
		ret = -ENOSYS;
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
