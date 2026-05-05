/*
 * user/libjnu/include/jnu_syscall.h - Tiny native JNU userspace ABI.
 *
 * v0.0.3 §2.2: Linux x86_64-compatible syscall numbers. Mirrors
 * kernel/include/jnu/syscall_nr.h.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

typedef unsigned long size_t;
typedef signed long ssize_t;
typedef signed long int64_t;
typedef unsigned long uint64_t;
typedef unsigned int uint32_t;

#define JNU_SYS_read         0
#define JNU_SYS_write        1
#define JNU_SYS_open         2
#define JNU_SYS_close        3
#define JNU_SYS_fstat        5
#define JNU_SYS_lseek        8
#define JNU_SYS_sched_yield 24
#define JNU_SYS_getpid      39
#define JNU_SYS_fork        57
#define JNU_SYS_execve      59
#define JNU_SYS_exit        60
#define JNU_SYS_wait4       61
#define JNU_SYS_fsync       74
#define JNU_SYS_ftruncate   77
#define JNU_SYS_rename      82
#define JNU_SYS_mkdir       83
#define JNU_SYS_rmdir       84
#define JNU_SYS_creat       85
#define JNU_SYS_unlink      87

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

struct jnu_stat {
	uint64_t ino;
	uint64_t size;
	uint32_t mode;
	uint32_t type;
};

long jnu_syscall0(long nr);
long jnu_syscall1(long nr, long a0);
long jnu_syscall2(long nr, long a0, long a1);
long jnu_syscall3(long nr, long a0, long a1, long a2);
long jnu_syscall4(long nr, long a0, long a1, long a2, long a3);

int close(int fd);
int execve(const char *path, char *const argv[], char *const envp[]);
void exit(int status) __attribute__((noreturn));
int fstat(int fd, void *st);
int fork(void);
int getpid(void);
int64_t lseek(int fd, int64_t off, int whence);
int open(const char *path, int flags);
ssize_t read(int fd, void *buf, size_t len);
int waitpid(int pid, int *status);
ssize_t write(int fd, const void *buf, size_t len);
int yield(void);
