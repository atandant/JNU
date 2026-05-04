/*
 * kernel/syscall/sys_arch_prctl.c — arch_prctl for FS/GS base.
 *
 * v0.0.3 §2.9: handles ARCH_SET_FS / ARCH_GET_FS / ARCH_SET_GS /
 * ARCH_GET_GS via the FSBASE/GSBASE MSRs.  This is load-bearing for
 * musl: TLS access through %fs:0x... reads errno and the thread-local
 * storage block.  The value is stored in struct task so context
 * switches preserve it via wrmsr(MSR_FS_BASE, task->fs_base).
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/cpu.h>
#include <jnu/errno.h>
#include <jnu/process.h>
#include <jnu/sched.h>
#include <jnu/syscall.h>
#include <jnu/types.h>
#include <jnu/usercopy.h>

#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

int64_t sys_arch_prctl(int code, uint64_t addr)
{
	struct task *t = sched_current();

	if (!t) {
		return -EINVAL;
	}

	switch (code) {
	case ARCH_SET_FS:
		if (addr >= USER_TOP) {
			return -EPERM;
		}
		t->fs_base = addr;
		wrmsr(MSR_FS_BASE, addr);
		return 0;

	case ARCH_GET_FS:
		if (!addr) {
			return -EINVAL;
		}
		return copy_to_user((void *)addr, &t->fs_base,
				    sizeof(t->fs_base));

	case ARCH_SET_GS:
		if (addr >= USER_TOP) {
			return -EPERM;
		}
		t->gs_base = addr;
		wrmsr(MSR_GS_BASE, addr);
		return 0;

	case ARCH_GET_GS:
		if (!addr) {
			return -EINVAL;
		}
		return copy_to_user((void *)addr, &t->gs_base,
				    sizeof(t->gs_base));

	default:
		return -EINVAL;
	}
}
