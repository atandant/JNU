/*
 * include/jnu/mm/paging.h — 4-level x86_64 paging operations.
 *
 * Each PTE is a 64-bit word. Flags below are the architectural bits;
 * the JNU virtual layout (§2.3) is enforced by the callers (vmm.c,
 * pmm.c).
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/base/types.h>

#define PAGE_SHIFT 12
#define PAGE_SIZE (1ull << PAGE_SHIFT)
#define PAGE_MASK (PAGE_SIZE - 1)

#define PAGE_HUGE_SHIFT 21
#define PAGE_HUGE_SIZE (1ull << PAGE_HUGE_SHIFT)

#define PTE_PRESENT (1ull << 0)
#define PTE_WRITE (1ull << 1)
#define PTE_USER (1ull << 2)
#define PTE_PWT (1ull << 3)
#define PTE_PCD (1ull << 4)
#define PTE_ACCESSED (1ull << 5)
#define PTE_DIRTY (1ull << 6)
#define PTE_HUGE (1ull << 7)
#define PTE_GLOBAL (1ull << 8)
#define PTE_NX (1ull << 63)

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ull

struct addr_space; /* forward; defined in <jnu/vmm.h> */

/*
 * Create the kernel-side initial PML4 by stealing one PMM page, copy the
 * Limine-installed mappings (HHDM + kernel image) over, switch CR3 to it,
 * and stash a pointer in `paging_kernel_pml4`. Must run after pmm_init().
 */
void paging_init(uint64_t hhdm_offset);

/* Translate a physical address to its HHDM virtual address. */
void *phys_to_virt(paddr_t p);
paddr_t virt_to_phys(void *v);

/*
 * Map `pages` 4 KiB pages from `phys` to `virt` in the address space's
 * PML4. Allocates intermediate page-tables from the PMM as needed.
 * Returns 0 on success or a negative errno.
 */
int paging_map(struct addr_space *space, vaddr_t virt, paddr_t phys,
	       size_t pages, uint64_t flags);

int paging_unmap(struct addr_space *space, vaddr_t virt, size_t pages);

int paging_protect(struct addr_space *space, vaddr_t virt, size_t pages,
		   uint64_t new_flags);

int paging_get_flags(struct addr_space *space, vaddr_t virt,
		     uint64_t *out_flags);

/*
 * For new address spaces: copy the kernel-half PDPT pointers from the
 * boot PML4 so kernel mappings are shared without per-space updates.
 */
void paging_clone_kernel_half(uint64_t *new_pml4);
void paging_destroy_user_half(uint64_t *pml4);

/* Flush a single virtual page from the TLB. */
static inline void paging_invlpg(vaddr_t v)
{
	__asm__ __volatile__("invlpg (%0)" ::"r"(v) : "memory");
}

/* Read CR2, the faulting address for #PF. */
static inline uint64_t paging_read_cr2(void)
{
	uint64_t v;
	__asm__ __volatile__("mov %%cr2, %0" : "=r"(v));
	return v;
}

/*
 * Ensure that `len` bytes starting at physical `base` are accessible
 * through the HHDM.  Limine may omit RESERVED memory-map regions
 * (BIOS ROM, ACPI tables) from the HHDM; this function creates 4 KiB
 * mappings for any pages not already covered — including pages already
 * reachable via 2 MiB or 1 GiB huge pages.
 *
 * Must be called after pmm_init() and paging_init().
 */
int paging_ensure_hhdm(paddr_t base, size_t len);

uint64_t *paging_kernel_pml4(void);
