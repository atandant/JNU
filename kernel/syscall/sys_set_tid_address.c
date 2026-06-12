/*
 * kernel/syscall/sys_set_tid_address.c — set_tid_address.
 *
 * v0.0.4: stores the clear_child_tid pointer on the calling task. On
 * thread exit the kernel writes 0 to this address and issues a
 * FUTEX_WAKE so a joiner blocked on the word can proceed.
 * Returns the caller's tid.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/base/types.h>
#include <jnu/kernel/sched.h>
#include <jnu/user/syscall.h>

int64_t sys_set_tid_address(void *tidptr)
{
	struct task *t = sched_current();

	if (!t) {
		return 1;
	}
	t->clear_child_tid = tidptr;
	return (int64_t)t->tid;
}
