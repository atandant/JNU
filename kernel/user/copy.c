/*
 * kernel/user/copy.c - Userspace pointer validation and copying.
 *
 * v0.0.2 centralizes all userspace memory access here. The helpers
 * validate the canonical low-half range and require present PTE_USER
 * mappings with the requested permissions before copying.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/paging.h>
#include <jnu/pmm.h>
#include <jnu/process.h>
#include <jnu/sched.h>
#include <jnu/string.h>
#include <jnu/usercopy.h>
#include <jnu/vmm.h>

static bool range_end(const void *uaddr, size_t len, uintptr_t *start,
		      uintptr_t *end)
{
	uintptr_t s = (uintptr_t)uaddr;
	uintptr_t e = s + len;

	if (len == 0) {
		*start = s;
		*end = s;
		return true;
	}
	if (e < s) {
		return false;
	}
	*start = s;
	*end = e;
	return true;
}

bool user_range_ok(const void *uaddr, size_t len)
{
	uintptr_t start;
	uintptr_t end;

	if (!range_end(uaddr, len, &start, &end)) {
		return false;
	}
	if (len == 0) {
		return start < USER_TOP;
	}
	if (start == 0 || start >= USER_TOP || end > USER_TOP) {
		return false;
	}
	return true;
}

int user_range_mapped(const void *uaddr, size_t len, bool write)
{
	struct task *task = sched_current();
	struct addr_space *space = vmm_kernel_space();
	uintptr_t start;
	uintptr_t end;

	if (!user_range_ok(uaddr, len)) {
		return -EFAULT;
	}
	if (len == 0) {
		return 0;
	}
	if (!range_end(uaddr, len, &start, &end)) {
		return -EFAULT;
	}
	if (task && task->process && task->process->space) {
		space = task->process->space;
	}

	start &= ~(uintptr_t)PAGE_MASK;
	for (uintptr_t page = start; page < end; page += PAGE_SIZE) {
		uint64_t flags;
		int err = paging_get_flags(space, page, &flags);
		if (err) {
			return -EFAULT;
		}
		if (!(flags & PTE_USER)) {
			return -EFAULT;
		}
		if (write && !(flags & PTE_WRITE)) {
			return -EFAULT;
		}
	}

	return 0;
}

int copy_from_user(void *dst, const void *usrc, size_t len)
{
	int err;

	if (len == 0) {
		return 0;
	}
	if (!dst) {
		return -EINVAL;
	}

	err = user_range_mapped(usrc, len, false);
	if (err) {
		return err;
	}

	memcpy(dst, usrc, len);
	return 0;
}

int copy_to_user(void *udst, const void *src, size_t len)
{
	int err;

	if (len == 0) {
		return 0;
	}
	if (!src) {
		return -EINVAL;
	}

	err = user_range_mapped(udst, len, true);
	if (err) {
		return err;
	}

	memcpy(udst, src, len);
	return 0;
}

int copy_string_from_user(char *dst, const char *usrc, size_t max)
{
	if (!dst || max == 0) {
		return -EINVAL;
	}

	for (size_t i = 0; i < max; i++) {
		int err = copy_from_user(&dst[i], &usrc[i], 1);
		if (err) {
			return err;
		}
		if (dst[i] == '\0') {
			return 0;
		}
	}

	dst[max - 1] = '\0';
	return -ENAMETOOLONG;
}

/* ------------------------------------------------------------------------- */
/* Selftest                                                                   */
/* ------------------------------------------------------------------------- */

#define USERCOPY_TEST_VA	0x0000000000400000ull

int usercopy_selftest(void)
{
	paddr_t pa;
	char dst[8];
	const char src[] = "jnu";
	int err;

	if (user_range_ok(NULL, 1)) {
		return -EINVAL;
	}
	if (user_range_ok((void *)USER_TOP, 1)) {
		return -EINVAL;
	}
	if (user_range_ok((void *)(USER_TOP - 1), 2)) {
		return -EINVAL;
	}

	pa = pmm_alloc_user_page();
	if (!pa) {
		return -ENOMEM;
	}

	err = vmm_map(vmm_kernel_space(), USERCOPY_TEST_VA, pa, 1,
		      VMA_READ | VMA_WRITE | VMA_USER);
	if (err) {
		goto fail_page;
	}

	if (*(volatile uint64_t *)USERCOPY_TEST_VA != 0) {
		err = -EINVAL;
		goto fail_unmap;
	}

	err = copy_to_user((void *)USERCOPY_TEST_VA, src, sizeof(src));
	if (err) {
		goto fail_unmap;
	}

	memset(dst, 0, sizeof(dst));
	err = copy_from_user(dst, (void *)USERCOPY_TEST_VA, sizeof(src));
	if (err) {
		goto fail_unmap;
	}
	if (memcmp(dst, src, sizeof(src)) != 0) {
		err = -EINVAL;
		goto fail_unmap;
	}

	err = copy_string_from_user(dst, (void *)USERCOPY_TEST_VA,
				    sizeof(dst));
	if (err) {
		goto fail_unmap;
	}
	if (strcmp(dst, src) != 0) {
		err = -EINVAL;
		goto fail_unmap;
	}

	err = copy_from_user(dst, (void *)0xFFFF800000000000ull, 1);
	if (err != -EFAULT) {
		err = -EINVAL;
		goto fail_unmap;
	}

	err = 0;

fail_unmap:
	vmm_unmap(vmm_kernel_space(), USERCOPY_TEST_VA, 1);
fail_page:
	pmm_free_pages(pa, 0);
	return err;
}
