/*
 * include/jnu/user/syscall.h - Native syscall dispatcher interface.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/base/types.h>

#define JNU_PATH_MAX 256

struct syscall_args {
	uint64_t nr;
	uint64_t arg0;
	uint64_t arg1;
	uint64_t arg2;
	uint64_t arg3;
	uint64_t arg4;
	uint64_t arg5;
};

/*
 * Trailing user state preserved by syscall_entry.S immediately after
 * the syscall_args struct on the kernel stack. Layout MUST match the
 * push order in kernel/arch/x86_64/syscall_entry.S:
 *
 *   push r12     (preserved user r12)         ← highest address
 *   push rsp     (saved user RSP from gs:0)
 *   push rcx     (user RIP, set by SYSCALL)
 *   push r11     (user RFLAGS, set by SYSCALL)
 *   ... struct syscall_args (rax + 6 args) ...   ← lowest, args points here
 *
 * Used by `sys_fork` to forge the child's iret state from the parent's
 * syscall return frame. Out-of-line sysret restoration is unaffected.
 * In memory after `struct syscall_args`: rflags, rip, rsp, r12, rbx,
 * rbp, r13, r14, r15.
 */
struct syscall_user_state {
	uint64_t rflags;
	uint64_t rip;
	uint64_t rsp;
	uint64_t r12;
	uint64_t rbx;
	uint64_t rbp;
	uint64_t r13;
	uint64_t r14;
	uint64_t r15;
};

struct syscall_frame {
	struct syscall_args args;
	struct syscall_user_state user;
};

static inline const struct syscall_user_state *
syscall_user_state_of(const struct syscall_args *args)
{
	return (const struct syscall_user_state *)(args + 1);
}

int64_t syscall_dispatch(const struct syscall_args *args);
int syscall_copy_path(char *dst, const char *upath);
int syscall_selftest(void);

int64_t sys_close(int fd);
int64_t sys_creat(const char *upath, int mode);
int64_t sys_execve(const struct syscall_args *args);
int64_t sys_exit(int status);
int64_t sys_fork(const struct syscall_args *args);
int64_t sys_fstat(int fd, void *ust);
int64_t sys_fsync(int fd);
int64_t sys_ftruncate(int fd, int64_t length);
int64_t sys_getpid(void);
int64_t sys_lseek(int fd, int64_t off, int whence);
int64_t sys_mmap(uint64_t addr, uint64_t length, int prot, int flags, int fd,
		 int64_t offset);
int64_t sys_mprotect(uint64_t addr, uint64_t length, int prot);
int64_t sys_munmap(uint64_t addr, uint64_t length);
int64_t sys_mkdir(const char *upath, int mode);
int64_t sys_open(const char *upath, int flags);
int64_t sys_read(int fd, void *ubuf, size_t len);
int64_t sys_rename(const char *uold, const char *unew);
int64_t sys_rmdir(const char *upath);
int64_t sys_unlink(const char *upath);
int64_t sys_waitpid(int pid, int *ustatus);
int64_t sys_write(int fd, const void *ubuf, size_t len);
int64_t sys_yield(void);

/* v0.0.3 Phase 3: musl-support syscalls (§2.9). */
int64_t sys_rt_sigaction(int signum, const void *act, void *oldact,
			 size_t sigsetsize);
int64_t sys_rt_sigprocmask(int how, const void *set, void *oldset,
			   size_t sigsetsize);
int64_t sys_ioctl(int fd, uint64_t request, uint64_t arg);
int64_t sys_writev(int fd, const void *uiov, int iovcnt);
int64_t sys_nanosleep(const void *ureq, void *urem);
int64_t sys_arch_prctl(int code, uint64_t addr);
int64_t sys_set_tid_address(void *tidptr);
int64_t sys_clock_gettime(int clockid, void *utp);
int64_t sys_exit_group(int status);
int64_t sys_getrandom(void *ubuf, size_t buflen, unsigned int flags);
