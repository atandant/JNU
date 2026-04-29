/*
 * kernel/arch/x86_64/paging.c — 4-level page-table operations.
 *
 * Supports 4 KiB and 2 MiB mappings. The kernel PML4 is the one Limine
 * left in CR3 — it already contains the HHDM and kernel-image mappings
 * we rely on, so cloning it would be busy-work for v0.0.1. We allocate
 * intermediate page-table pages from the PMM lazily as map operations
 * descend through the levels.
 *
 * Reference: Intel SDM Vol. 3 §4.5 (4-level paging).
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/compiler.h>
#include <jnu/errno.h>
#include <jnu/klog.h>
#include <jnu/paging.h>
#include <jnu/panic.h>
#include <jnu/pmm.h>
#include <jnu/string.h>
#include <jnu/types.h>
#include <jnu/vmm.h>

static uint64_t hhdm_offset;
static uint64_t *kernel_pml4;
static paddr_t   kernel_pml4_phys;

void *phys_to_virt(paddr_t p)
{
	return (void *)(uintptr_t)(p + hhdm_offset);
}

paddr_t virt_to_phys(void *v)
{
	return (paddr_t)((uintptr_t)v - hhdm_offset);
}

static uint64_t read_cr3(void)
{
	uint64_t v;
	__asm__ __volatile__ ("mov %%cr3, %0" : "=r"(v));
	return v;
}

void paging_init(uint64_t hhdm)
{
	hhdm_offset = hhdm;
	kernel_pml4_phys = read_cr3() & 0x000FFFFFFFFFF000ull;
	kernel_pml4 = phys_to_virt(kernel_pml4_phys);

	pr_info("paging: kernel PML4 at phys 0x%lx\n",
		(unsigned long)kernel_pml4_phys);
}

uint64_t *paging_kernel_pml4(void)
{
	return kernel_pml4;
}

/* ------------------------------------------------------------------------- */
/* Walk helpers                                                               */
/* ------------------------------------------------------------------------- */

static uint64_t *table_for(uint64_t *parent, unsigned idx, bool create,
			   uint64_t pflags)
{
	uint64_t e = parent[idx];
	if (!(e & PTE_PRESENT)) {
		if (!create) {
			return NULL;
		}
		paddr_t pa = pmm_alloc_pages(0);
		if (!pa) {
			return NULL;
		}
		uint64_t *t = phys_to_virt(pa);
		memset(t, 0, PAGE_SIZE);
		parent[idx] = pa | PTE_PRESENT | PTE_WRITE | pflags;
		return t;
	}
	if (e & PTE_HUGE) {
		return NULL;	/* mapping conflict at higher level */
	}
	return phys_to_virt(e & PTE_ADDR_MASK);
}

static unsigned pml4_idx(vaddr_t v) { return (v >> 39) & 0x1FF; }
static unsigned pdpt_idx(vaddr_t v) { return (v >> 30) & 0x1FF; }
static unsigned pd_idx(vaddr_t v)   { return (v >> 21) & 0x1FF; }
static unsigned pt_idx(vaddr_t v)   { return (v >> 12) & 0x1FF; }

/* ------------------------------------------------------------------------- */
/* Map / Unmap / Protect                                                      */
/* ------------------------------------------------------------------------- */

int paging_map(struct addr_space *space, vaddr_t virt, paddr_t phys,
	       size_t pages, uint64_t flags)
{
	uint64_t *pml4 = (space && space->pml4) ? space->pml4 : kernel_pml4;
	uint64_t intermediate_flags =
		(flags & PTE_USER) ? PTE_USER : 0;

	for (size_t i = 0; i < pages; i++) {
		vaddr_t v = virt + i * PAGE_SIZE;
		paddr_t p = phys + i * PAGE_SIZE;

		uint64_t *pdpt = table_for(pml4, pml4_idx(v), true,
					   intermediate_flags);
		if (!pdpt) { return -ENOMEM; }
		uint64_t *pd   = table_for(pdpt, pdpt_idx(v), true,
					   intermediate_flags);
		if (!pd)   { return -ENOMEM; }
		uint64_t *pt   = table_for(pd, pd_idx(v), true,
					   intermediate_flags);
		if (!pt)   { return -ENOMEM; }

		pt[pt_idx(v)] = (p & PTE_ADDR_MASK) | PTE_PRESENT | flags;
		paging_invlpg(v);
	}
	return 0;
}

int paging_unmap(struct addr_space *space, vaddr_t virt, size_t pages)
{
	uint64_t *pml4 = (space && space->pml4) ? space->pml4 : kernel_pml4;

	for (size_t i = 0; i < pages; i++) {
		vaddr_t v = virt + i * PAGE_SIZE;
		uint64_t *pdpt = table_for(pml4, pml4_idx(v), false, 0);
		if (!pdpt) { continue; }
		uint64_t *pd   = table_for(pdpt, pdpt_idx(v), false, 0);
		if (!pd)   { continue; }
		uint64_t *pt   = table_for(pd, pd_idx(v), false, 0);
		if (!pt)   { continue; }

		pt[pt_idx(v)] = 0;
		paging_invlpg(v);
	}
	return 0;
}

int paging_protect(struct addr_space *space, vaddr_t virt, size_t pages,
		   uint64_t new_flags)
{
	uint64_t *pml4 = (space && space->pml4) ? space->pml4 : kernel_pml4;

	for (size_t i = 0; i < pages; i++) {
		vaddr_t v = virt + i * PAGE_SIZE;
		uint64_t *pdpt = table_for(pml4, pml4_idx(v), false, 0);
		if (!pdpt) { return -ENOENT; }
		uint64_t *pd   = table_for(pdpt, pdpt_idx(v), false, 0);
		if (!pd)   { return -ENOENT; }
		uint64_t *pt   = table_for(pd, pd_idx(v), false, 0);
		if (!pt)   { return -ENOENT; }

		uint64_t e = pt[pt_idx(v)];
		if (!(e & PTE_PRESENT)) { return -ENOENT; }

		pt[pt_idx(v)] = (e & PTE_ADDR_MASK) | PTE_PRESENT | new_flags;
		paging_invlpg(v);
	}
	return 0;
}

void paging_clone_kernel_half(uint64_t *new_pml4)
{
	for (unsigned i = 256; i < 512; i++) {
		new_pml4[i] = kernel_pml4[i];
	}
}

/*
 * Ensure a physical range is accessible through the HHDM.
 *
 * Limine only maps memory-map entries it considers "available" into the
 * HHDM; RESERVED regions (BIOS ROM, ACPI tables at 0xE0000–0xFFFFF,
 * LAPIC/IOAPIC MMIO) are typically omitted.  This function walks the
 * page-table hierarchy for each 4 KiB page in [base, base+len), skips
 * pages already reachable via 1 GiB or 2 MiB huge pages, and creates
 * 4 KiB PTE entries for the rest.
 *
 * Returns 0 on success, negative errno on allocation failure.
 */
int paging_ensure_hhdm(paddr_t base, size_t len)
{
	paddr_t start = base & ~(paddr_t)PAGE_MASK;
	paddr_t end   = (base + len + PAGE_MASK) & ~(paddr_t)PAGE_MASK;

	for (paddr_t pa = start; pa < end; pa += PAGE_SIZE) {
		vaddr_t va = (vaddr_t)(pa + hhdm_offset);

		/* PML4 */
		unsigned i4 = pml4_idx(va);
		if (!(kernel_pml4[i4] & PTE_PRESENT)) {
			paddr_t tpa = pmm_alloc_pages(0);
			if (!tpa)
				return -ENOMEM;
			memset(phys_to_virt(tpa), 0, PAGE_SIZE);
			kernel_pml4[i4] = tpa | PTE_PRESENT | PTE_WRITE;
		}

		/* PDPT — skip if a 1 GiB huge page already covers it */
		uint64_t *pdpt = phys_to_virt(kernel_pml4[i4] & PTE_ADDR_MASK);
		unsigned i3 = pdpt_idx(va);
		if ((pdpt[i3] & PTE_PRESENT) && (pdpt[i3] & PTE_HUGE))
			continue;
		if (!(pdpt[i3] & PTE_PRESENT)) {
			paddr_t tpa = pmm_alloc_pages(0);
			if (!tpa)
				return -ENOMEM;
			memset(phys_to_virt(tpa), 0, PAGE_SIZE);
			pdpt[i3] = tpa | PTE_PRESENT | PTE_WRITE;
		}

		/* PD — skip if a 2 MiB huge page already covers it */
		uint64_t *pd = phys_to_virt(pdpt[i3] & PTE_ADDR_MASK);
		unsigned i2 = pd_idx(va);
		if ((pd[i2] & PTE_PRESENT) && (pd[i2] & PTE_HUGE))
			continue;
		if (!(pd[i2] & PTE_PRESENT)) {
			paddr_t tpa = pmm_alloc_pages(0);
			if (!tpa)
				return -ENOMEM;
			memset(phys_to_virt(tpa), 0, PAGE_SIZE);
			pd[i2] = tpa | PTE_PRESENT | PTE_WRITE;
		}

		/* PT — create the 4 KiB mapping if absent */
		uint64_t *pt = phys_to_virt(pd[i2] & PTE_ADDR_MASK);
		unsigned i1 = pt_idx(va);
		if (!(pt[i1] & PTE_PRESENT)) {
			pt[i1] = (pa & PTE_ADDR_MASK) | PTE_PRESENT
				 | PTE_WRITE;
			paging_invlpg(va);
		}
	}
	return 0;
}
