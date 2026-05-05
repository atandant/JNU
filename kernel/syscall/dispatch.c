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

static int64_t wrap_creat(const struct syscall_args *a)
{
	return sys_creat((const char *)a->arg0, (int)a->arg1);
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

static int64_t wrap_fsync(const struct syscall_args *a)
{
	return sys_fsync((int)a->arg0);
}

static int64_t wrap_ftruncate(const struct syscall_args *a)
{
	return sys_ftruncate((int)a->arg0, (int64_t)a->arg1);
}

static int64_t wrap_unlink(const struct syscall_args *a)
{
	return sys_unlink((const char *)a->arg0);
}

static int64_t wrap_mkdir(const struct syscall_args *a)
{
	return sys_mkdir((const char *)a->arg0, (int)a->arg1);
}

static int64_t wrap_rmdir(const struct syscall_args *a)
{
	return sys_rmdir((const char *)a->arg0);
}

static int64_t wrap_rename(const struct syscall_args *a)
{
	return sys_rename((const char *)a->arg0, (const char *)a->arg1);
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

static int64_t wrap_mmap(const struct syscall_args *a)
{
	return sys_mmap(a->arg0, a->arg1, (int)a->arg2, (int)a->arg3,
			(int)a->arg4, (int64_t)a->arg5);
}

static int64_t wrap_mprotect(const struct syscall_args *a)
{
	return sys_mprotect(a->arg0, a->arg1, (int)a->arg2);
}

static int64_t wrap_munmap(const struct syscall_args *a)
{
	return sys_munmap(a->arg0, a->arg1);
}

static int64_t wrap_rt_sigaction(const struct syscall_args *a)
{
	return sys_rt_sigaction((int)a->arg0, (const void *)a->arg1,
			       (void *)a->arg2, (size_t)a->arg3);
}

static int64_t wrap_rt_sigprocmask(const struct syscall_args *a)
{
	return sys_rt_sigprocmask((int)a->arg0, (const void *)a->arg1,
				 (void *)a->arg2, (size_t)a->arg3);
}

static int64_t wrap_ioctl(const struct syscall_args *a)
{
	return sys_ioctl((int)a->arg0, a->arg1, a->arg2);
}

static int64_t wrap_writev(const struct syscall_args *a)
{
	return sys_writev((int)a->arg0, (const void *)a->arg1, (int)a->arg2);
}

static int64_t wrap_nanosleep(const struct syscall_args *a)
{
	return sys_nanosleep((const void *)a->arg0, (void *)a->arg1);
}

static int64_t wrap_arch_prctl(const struct syscall_args *a)
{
	return sys_arch_prctl((int)a->arg0, a->arg1);
}

static int64_t wrap_set_tid_address(const struct syscall_args *a)
{
	return sys_set_tid_address((void *)a->arg0);
}

static int64_t wrap_clock_gettime(const struct syscall_args *a)
{
	return sys_clock_gettime((int)a->arg0, (void *)a->arg1);
}

static int64_t wrap_exit_group(const struct syscall_args *a)
{
	return sys_exit_group((int)a->arg0);
}

static int64_t wrap_getrandom(const struct syscall_args *a)
{
	return sys_getrandom((void *)a->arg0, (size_t)a->arg1,
			     (unsigned int)a->arg2);
}

/*
 * Sparse dispatch table.  Index == Linux x86_64 syscall number.
 * NULL entries return -ENOSYS.
 */
static const syscall_handler_t syscall_table[JNU_SYS_MAX + 1] = {
	[JNU_SYS_read]           = wrap_read,
	[JNU_SYS_write]          = wrap_write,
	[JNU_SYS_open]           = wrap_open,
	[JNU_SYS_close]          = wrap_close,
	[JNU_SYS_fstat]          = wrap_fstat,
	[JNU_SYS_lseek]          = wrap_lseek,
	[JNU_SYS_mmap]           = wrap_mmap,
	[JNU_SYS_mprotect]       = wrap_mprotect,
	[JNU_SYS_munmap]         = wrap_munmap,
	[JNU_SYS_rt_sigaction]   = wrap_rt_sigaction,
	[JNU_SYS_rt_sigprocmask] = wrap_rt_sigprocmask,
	[JNU_SYS_ioctl]          = wrap_ioctl,
	[JNU_SYS_writev]         = wrap_writev,
	[JNU_SYS_sched_yield]    = wrap_sched_yield,
	[JNU_SYS_nanosleep]      = wrap_nanosleep,
	[JNU_SYS_getpid]         = wrap_getpid,
	[JNU_SYS_fork]           = wrap_fork,
	[JNU_SYS_execve]         = wrap_execve,
	[JNU_SYS_exit]           = wrap_exit,
	[JNU_SYS_wait4]          = wrap_wait4,
	[JNU_SYS_fsync]          = wrap_fsync,
	[JNU_SYS_ftruncate]      = wrap_ftruncate,
	[JNU_SYS_rename]         = wrap_rename,
	[JNU_SYS_mkdir]          = wrap_mkdir,
	[JNU_SYS_rmdir]          = wrap_rmdir,
	[JNU_SYS_creat]          = wrap_creat,
	[JNU_SYS_unlink]         = wrap_unlink,
	[JNU_SYS_arch_prctl]     = wrap_arch_prctl,
	[JNU_SYS_set_tid_address] = wrap_set_tid_address,
	[JNU_SYS_clock_gettime]  = wrap_clock_gettime,
	[JNU_SYS_exit_group]     = wrap_exit_group,
	[JNU_SYS_getrandom]      = wrap_getrandom,
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
