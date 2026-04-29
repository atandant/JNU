/*
 * include/jnu/vmm.h — Virtual memory manager: address spaces and VMAs.
 *
 * One `struct addr_space` per process (in v0.0.1 there is only the
 * kernel space). Wraps a PML4 plus a red-black tree of VMAs.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/rbtree.h>
#include <jnu/types.h>

#define VMA_READ		(1u << 0)
#define VMA_WRITE		(1u << 1)
#define VMA_EXEC		(1u << 2)
#define VMA_USER		(1u << 3)

struct vma {
	struct rb_node	rb;
	vaddr_t		start;
	vaddr_t		end;	/* exclusive */
	uint32_t	flags;
};

struct addr_space {
	uint64_t	*pml4;		/* HHDM-virt pointer */
	paddr_t		pml4_phys;
	struct rb_root	vmas;
};

void vmm_init(void);

struct addr_space *vmm_kernel_space(void);

/*
 * Map `pages` × 4 KiB starting at `virt` to `phys` in `space`. Updates
 * both the page tables and the VMA tree. Returns 0 / -errno.
 */
int vmm_map(struct addr_space *space, vaddr_t virt, paddr_t phys,
	    size_t pages, uint32_t flags);

int vmm_unmap(struct addr_space *space, vaddr_t virt, size_t pages);

int vmm_protect(struct addr_space *space, vaddr_t virt, size_t pages,
		uint32_t new_flags);

void vmm_switch_to(struct addr_space *space);

int vmm_selftest(void);
