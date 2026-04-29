/*
 * kernel/kernel/sched.c — Scheduler stub for v0.0.1.
 *
 * v0.0.1 has no scheduler. Kernel_main runs to completion and idles.
 * This file exists so headers expecting `<jnu/sched.h>` can link.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/klog.h>
#include <jnu/sched.h>

void sched_init(void)
{
	pr_info("sched: stub (no scheduler in v0.0.1)\n");
}
