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

#include <jnu/arch/cpu.h>
#include <jnu/arch/irq.h>
#include <jnu/base/types.h>
#include <jnu/drivers/ata.h>
#include <jnu/drivers/lapic_timer.h>
#include <jnu/drivers/pci.h>
#include <jnu/drivers/virtio_blk.h>
#include <jnu/fs/initramfs.h>
#include <jnu/fs/minix.h>
#include <jnu/fs/vfs.h>
#include <jnu/kernel/elf64.h>
#include <jnu/kernel/futex.h>
#include <jnu/kernel/process.h>
#include <jnu/kernel/sched.h>
#include <jnu/kernel/selftest.h>
#include <jnu/lib/klog.h>
#include <jnu/lib/mutex.h>
#include <jnu/lib/rbtree.h>
#include <jnu/lib/spinlock.h>
#include <jnu/mm/pmm.h>
#include <jnu/mm/slab.h>
#include <jnu/mm/vmm.h>
#include <jnu/user/fd.h>
#include <jnu/user/syscall.h>
#include <jnu/user/usercopy.h>

static const struct selftest tests[] = {
    {"spinlock", spinlock_selftest},
    {"rbtree", rbtree_selftest},
    {"pmm", pmm_selftest},
    {"pmm_zerofree", pmm_zerofree_selftest},
    {"fpu", fpu_selftest},
    {"vmm", vmm_selftest},
    {"slab", slab_selftest},
    {"initramfs", initramfs_selftest},
    {"usercopy", usercopy_selftest},
    {"sched", sched_selftest},
    {"mutex", mutex_selftest},
    {"futex", futex_selftest},
    {"lapic_timer", lapic_timer_selftest},
    {"process", process_selftest},
    {"file_refcount", file_refcount_selftest},
    {"clone_space", clone_space_selftest},
    {"elf64", elf64_selftest},
    {"syscall", syscall_selftest},
    {"irq", irq_selftest},
    {"pci", pci_selftest},
    {"ata", ata_selftest},
    {"virtio_blk", virtio_blk_selftest},
    {"bufcache", bufcache_selftest},
    {"minix_bitmap", minix_bitmap_selftest},
    {"minix_write", minix_write_selftest},
    {"minix_dir", minix_dir_selftest},
    {"minix_fsync", minix_fsync_selftest},
    {"vfs", vfs_selftest},
    {"minix", minix_selftest},
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
			pr_err("selftest: %s [FAIL] err=%d\n", tests[i].name,
			       err);
			failures++;
		}
	}

	if (failures == 0) {
		pr_info("selftest: all %u green\n",
			(unsigned)(sizeof(tests) / sizeof(tests[0])));
	} else {
		pr_err("selftest: %d failure%s\n", failures,
		       failures == 1 ? "" : "s");
	}

	bufcache_log_stats();
	return failures;
}
