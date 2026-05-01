/*
 * user/libjnu/include/jnu_syscall.h - Tiny native JNU userspace ABI.
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

#define JNU_SYS_exit 0
#define JNU_SYS_write 1
#define JNU_SYS_read 2
#define JNU_SYS_open 3
#define JNU_SYS_close 4
#define JNU_SYS_lseek 5
#define JNU_SYS_getpid 6
#define JNU_SYS_yield 7
#define JNU_SYS_fstat 8
#define JNU_SYS_spawn 9
#define JNU_SYS_waitpid 10

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

int close(int fd);
void exit(int status) __attribute__((noreturn));
int fstat(int fd, void *st);
int getpid(void);
int64_t lseek(int fd, int64_t off, int whence);
int open(const char *path, int flags);
ssize_t read(int fd, void *buf, size_t len);
int spawn(const char *path, char *const argv[]);
int waitpid(int pid, int *status);
ssize_t write(int fd, const void *buf, size_t len);
int yield(void);
