/*
 * kernel/kernel/selftest.c — Boot-time selftest harness.
 *
 * Calls each registered subsystem selftest in order, logs the result,
 * and returns the failure count. Called from `kernel_main` when the
 * kernel cmdline contains `selftest=1`.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/ata.h>
#include <jnu/klog.h>
#include <jnu/pci.h>
#include <jnu/pmm.h>
#include <jnu/rbtree.h>
#include <jnu/selftest.h>
#include <jnu/slab.h>
#include <jnu/spinlock.h>
#include <jnu/types.h>
#include <jnu/vfs.h>
#include <jnu/minix.h>
#include <jnu/vmm.h>

static const struct selftest tests[] = {
	{ "spinlock",	spinlock_selftest },
	{ "rbtree",	rbtree_selftest   },
	{ "pmm",	pmm_selftest      },
	{ "vmm",	vmm_selftest      },
	{ "slab",	slab_selftest     },
	{ "pci",	pci_selftest      },
	{ "ata",	ata_selftest      },
	{ "vfs",	vfs_selftest      },
	{ "minix",	minix_selftest    },
};

int selftest_run_all(void)
{
	int failures = 0;
	pr_info("selftest: running %u tests\n",
		(unsigned)(sizeof(tests) / sizeof(tests[0])));

	for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		int err = tests[i].run();
		if (err == 0) {
			pr_info("selftest: %s [ OK ]\n", tests[i].name);
		} else {
			pr_err("selftest: %s [FAIL] err=%d\n",
			       tests[i].name, err);
			failures++;
		}
	}

	if (failures == 0) {
		pr_info("selftest: all %u green\n",
			(unsigned)(sizeof(tests) / sizeof(tests[0])));
	} else {
		pr_err("selftest: %d failure%s\n",
		       failures, failures == 1 ? "" : "s");
	}

	return failures;
}
