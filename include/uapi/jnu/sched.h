/*
 * include/uapi/jnu/sched.h — Linux-compatible clone(2) flag constants.
 *
 * v0.0.4: flag values match Linux x86_64 so musl's pthread_create()
 * passes the same bits it would on Linux. JNU only implements the
 * thread-creation subset (CLONE_VM | CLONE_THREAD + the TLS/tid
 * helpers); see kernel/syscall/sys_clone.c for the accepted set.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#define CLONE_VM 0x00000100		/* share address space */
#define CLONE_FS 0x00000200		/* share filesystem info */
#define CLONE_FILES 0x00000400		/* share fd table */
#define CLONE_SIGHAND 0x00000800	/* share signal handlers */
#define CLONE_THREAD 0x00010000		/* same thread group (tgid) */
#define CLONE_SYSVSEM 0x00040000	/* share System V SEM_UNDO state */
#define CLONE_SETTLS 0x00080000		/* set TLS (FS base) from `tls` arg */
#define CLONE_PARENT_SETTID 0x00100000	/* write child tid to parent_tid */
#define CLONE_CHILD_CLEARTID 0x00200000 /* clear+futex-wake child_tid on exit  \
					 */
#define CLONE_DETACHED 0x00400000	/* (historical, ignored) */
#define CLONE_CHILD_SETTID 0x01000000	/* write child tid to child_tid */
