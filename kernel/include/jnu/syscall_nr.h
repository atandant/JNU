/*
 * include/jnu/syscall_nr.h - Native JNU syscall numbers.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#define JNU_SYS_exit 0
#define JNU_SYS_write 1
#define JNU_SYS_read 2
#define JNU_SYS_open 3
#define JNU_SYS_close 4
#define JNU_SYS_lseek 5
#define JNU_SYS_getpid 6
#define JNU_SYS_yield 7
#define JNU_SYS_fstat 8
/* 9 is retired; it was JNU_SYS_spawn. */
#define JNU_SYS_spawn 9
#define JNU_SYS_waitpid 10
#define JNU_SYS_fork 11
#define JNU_SYS_execve 12

#define JNU_SYS_MAX 12
