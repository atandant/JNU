/*
 * include/jnu/syscall.h - Native syscall dispatcher interface.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>

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
 */
struct syscall_user_state {
	uint64_t rflags;
	uint64_t rip;
	uint64_t rsp;
	uint64_t r12;
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
int64_t sys_exit(int status);
int64_t sys_fork(const struct syscall_args *args);
int64_t sys_fstat(int fd, void *ust);
int64_t sys_getpid(void);
int64_t sys_lseek(int fd, int64_t off, int whence);
int64_t sys_open(const char *upath, int flags);
int64_t sys_read(int fd, void *ubuf, size_t len);
int64_t sys_spawn(const char *upath, char *const *uargv);
int64_t sys_waitpid(int pid, int *ustatus);
int64_t sys_write(int fd, const void *ubuf, size_t len);
int64_t sys_yield(void);
