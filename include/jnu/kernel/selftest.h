/*
 * include/jnu/kernel/selftest.h — Boot-time selftest registry.
 *
 * Each subsystem owns a `<subsys>_selftest()` function that returns 0
 * on success or a negative errno. The selftest harness in kernel_main
 * calls each in order when `selftest=1` is on the kernel cmdline.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/base/types.h>

struct selftest {
	const char *name;
	int (*run)(void);
};

/*
 * Run the entire selftest table. Logs each result via pr_info / pr_err.
 * Returns the number of failures (0 == all green).
 */
int selftest_run_all(void);
