/*
 * include/jnu/syscall_nr.h - Native JNU syscall numbers.
 *
 * v0.0.3 §2.2: Linux x86_64-compatible numbering for every syscall
 * present in both. The contract — register layout, semantics, error
 * handling — remains JNU's. Only the integers match. JNU-private
 * syscalls (none yet) use numbers in [1024, 1535].
 *
 * Supersedes jnuspec2.md §2.4.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#define JNU_SYS_read            0
#define JNU_SYS_write           1
#define JNU_SYS_open            2
#define JNU_SYS_close           3
#define JNU_SYS_fstat           5
#define JNU_SYS_lseek           8
#define JNU_SYS_mmap            9
#define JNU_SYS_mprotect       10
#define JNU_SYS_munmap         11
#define JNU_SYS_rt_sigaction   13
#define JNU_SYS_rt_sigprocmask 14
#define JNU_SYS_ioctl          16
#define JNU_SYS_writev         20
#define JNU_SYS_sched_yield    24
#define JNU_SYS_nanosleep      35
#define JNU_SYS_getpid         39
#define JNU_SYS_fork           57
#define JNU_SYS_execve         59
#define JNU_SYS_exit           60
#define JNU_SYS_wait4          61
#define JNU_SYS_fsync          74
#define JNU_SYS_ftruncate      77
#define JNU_SYS_rename         82
#define JNU_SYS_mkdir          83
#define JNU_SYS_rmdir          84
#define JNU_SYS_creat          85
#define JNU_SYS_unlink         87
#define JNU_SYS_arch_prctl    158
#define JNU_SYS_set_tid_address 218
#define JNU_SYS_clock_gettime 228
#define JNU_SYS_exit_group    231
#define JNU_SYS_getrandom     318

#define JNU_SYS_MAX           318
