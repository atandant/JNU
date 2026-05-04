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

#define VMA_READ (1u << 0)
#define VMA_WRITE (1u << 1)
#define VMA_EXEC (1u << 2)
#define VMA_USER (1u << 3)

struct vma {
	struct rb_node rb;
	vaddr_t start;
	vaddr_t end; /* exclusive */
	uint32_t flags;
};

struct addr_space {
	uint64_t *pml4; /* HHDM-virt pointer */
	paddr_t pml4_phys;
	struct rb_root vmas;
};

void vmm_init(void);

struct addr_space *vmm_kernel_space(void);
struct addr_space *vmm_create_space(void);
void vmm_destroy_space(struct addr_space *space);

/*
 * CoW clone: share every user-side PTE between `src` and a freshly
 * allocated destination, with PTE_WRITE cleared and refcounts bumped.
 * The destination VMA preserves the logical writability (VMA_WRITE).
 * Write faults are resolved by vmm_handle_cow_fault().  Returns 0 /
 * -errno.
 */
int vmm_clone_space(struct addr_space *src, struct addr_space **out);
int clone_space_selftest(void);

/*
 * v0.0.3 §2.8: create an anonymous private VMA in `space`.  PTEs are
 * NOT installed; the lazy zero-fill #PF handler materialises pages on
 * first access.  `prot` uses PROT_* flags, `flags` uses MAP_* flags.
 * On success the chosen base address is written to `*addr_out`.
 */
int vmm_map_anonymous(struct addr_space *space, vaddr_t addr, size_t length,
		      uint32_t prot, uint32_t flags, vaddr_t *addr_out);

/*
 * Attempt to resolve a CoW write fault at `va` in `space`.  Called from
 * the #PF handler when all of the following hold:
 *
 *   1. The fault is from user mode.
 *   2. The error code has PF_EC_W (write) and PF_EC_P (present).
 *   3. vma_find() returns a VMA with VMA_WRITE.
 *
 * Returns 0 if resolved, negative errno otherwise.
 */
int vmm_handle_cow_fault(struct addr_space *space, vaddr_t va);

/*
 * v0.0.3 §2.5: resolve a lazy zero-fill fault.  Called from the #PF
 * handler when the PTE is absent and a VMA covers the address.
 * `ec` is the raw x86-64 #PF error code.
 */
int vmm_handle_lazy_fault(struct addr_space *space, const struct vma *v,
			  vaddr_t va, uint32_t ec);
/*
 * Map `pages` × 4 KiB starting at `virt` to `phys` in `space`. Updates
 * both the page tables and the VMA tree. Returns 0 / -errno.
 */
int vmm_map(struct addr_space *space, vaddr_t virt, paddr_t phys, size_t pages,
	    uint32_t flags);

int vmm_unmap(struct addr_space *space, vaddr_t virt, size_t pages);

int vmm_protect(struct addr_space *space, vaddr_t virt, size_t pages,
		uint32_t new_flags);

void vmm_switch_to(struct addr_space *space);

int vmm_selftest(void);
