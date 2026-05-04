/*
 * kernel/syscall/sys_ioctl.c — ioctl stub.
 *
 * v0.0.3 §2.9: returns -ENOTTY for every request, regardless of fd.
 * musl's stdio probes terminal attributes via ioctl(TIOCGWINSZ) and
 * falls back gracefully when the call fails.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/syscall.h>
#include <jnu/types.h>

int64_t sys_ioctl(int fd, uint64_t request, uint64_t arg)
{
	(void)fd;
	(void)request;
	(void)arg;
	return -ENOTTY;
}
