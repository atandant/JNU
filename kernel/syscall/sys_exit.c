/*
 * kernel/syscall/sys_exit.c - exit syscall.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/kernel/process.h>
#include <jnu/kernel/sched.h>
#include <jnu/user/syscall.h>

int64_t sys_exit(int status)
{
	/*
	 * v0.0.4: exit only the calling thread. If it is the last thread
	 * in its group, process_thread_exit() performs full process
	 * teardown; otherwise the thread self-reaps as detached. Never
	 * returns. (exit_group() terminates the whole group instead.)
	 */
	process_thread_exit(status);
	return 0;
}
