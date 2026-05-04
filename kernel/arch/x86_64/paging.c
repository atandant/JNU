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
static paddr_t kernel_pml4_phys;

void *phys_to_virt(paddr_t p) { return (void *)(uintptr_t)(p + hhdm_offset); }

paddr_t virt_to_phys(void *v) { return (paddr_t)((uintptr_t)v - hhdm_offset); }

static uint64_t read_cr3(void)
{
	uint64_t v;
	__asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
	return v;
}

/*
 * Set NX on every writable PTE under PML4 index 511 (the kernel image
 * range, 0xFFFFFFFF80000000+). The kernel relies on Limine's per-PHDR
 * permissions for its initial mapping, but spec §2.4 mandates W^X and
 * a future Limine change or a misconfigured linker script could leave
 * .data/.bss mapped writable+executable. Walk the kernel image once at
 * boot and assert W^X by promoting any writable mapping to NX. Read-
 * only mappings (.text, .rodata) are left untouched so kernel code can
 * still execute.
 */
static void enforce_nx_on_kernel_image(void)
{
	unsigned i4 = 511;

	if (!(kernel_pml4[i4] & PTE_PRESENT))
		return;

	uint64_t *pdpt = phys_to_virt(kernel_pml4[i4] & PTE_ADDR_MASK);
	for (unsigned i3 = 0; i3 < 512; i3++) {
		if (!(pdpt[i3] & PTE_PRESENT))
			continue;

		if (pdpt[i3] & PTE_HUGE) {
			if (pdpt[i3] & PTE_WRITE)
				pdpt[i3] |= PTE_NX;
			continue;
		}

		uint64_t *pd = phys_to_virt(pdpt[i3] & PTE_ADDR_MASK);
		for (unsigned i2 = 0; i2 < 512; i2++) {
			if (!(pd[i2] & PTE_PRESENT))
				continue;

			if (pd[i2] & PTE_HUGE) {
				if (pd[i2] & PTE_WRITE)
					pd[i2] |= PTE_NX;
				continue;
			}

			uint64_t *pt = phys_to_virt(pd[i2] & PTE_ADDR_MASK);
			for (unsigned i1 = 0; i1 < 512; i1++) {
				if ((pt[i1] & PTE_PRESENT) &&
				    (pt[i1] & PTE_WRITE)) {
					pt[i1] |= PTE_NX;
				}
			}
		}
	}
}

static void enforce_nx_on_hhdm(void)
{
	/*
	 * Walk PML4 entries from 256 to 510 (covering 0xFFFF800000000000 to
	 * 0xFFFFFF7FFFFFFFFF). This explicitly excludes index 511 where the
	 * kernel image (.text, .rodata, etc.) resides. Limine maps the HHDM
	 * in this range without the NX bit; we enforce W^X by setting it.
	 */
	for (unsigned i4 = 256; i4 < 511; i4++) {
		if (!(kernel_pml4[i4] & PTE_PRESENT))
			continue;

		uint64_t *pdpt = phys_to_virt(kernel_pml4[i4] & PTE_ADDR_MASK);
		for (unsigned i3 = 0; i3 < 512; i3++) {
			if (!(pdpt[i3] & PTE_PRESENT))
				continue;

			if (pdpt[i3] & PTE_HUGE) {
				pdpt[i3] |= PTE_NX;
				continue;
			}

			uint64_t *pd = phys_to_virt(pdpt[i3] & PTE_ADDR_MASK);
			for (unsigned i2 = 0; i2 < 512; i2++) {
				if (!(pd[i2] & PTE_PRESENT))
					continue;

				if (pd[i2] & PTE_HUGE) {
					pd[i2] |= PTE_NX;
					continue;
				}

				uint64_t *pt =
				    phys_to_virt(pd[i2] & PTE_ADDR_MASK);
				for (unsigned i1 = 0; i1 < 512; i1++) {
					if (pt[i1] & PTE_PRESENT) {
						pt[i1] |= PTE_NX;
					}
				}
			}
		}
	}

	/* Reload CR3 to flush all TLB entries for modified page structures */
	__asm__ __volatile__("mov %0, %%cr3" ::"r"(kernel_pml4_phys)
			     : "memory");
}

/*
 * Eagerly populate every kernel-half PML4 slot (256..511) that Limine
 * left empty. Each cloned process address space gets a shallow copy of
 * these slots via paging_clone_kernel_half(); if a top-level slot is
 * ever filled in *after* a process is cloned, that process's PML4 has
 * a stale 0 there and any kernel access to the new region from that
 * task triggers a kernel-mode #PF, which exceptions_handle() escalates
 * to panic(). Pre-populating once at boot means the only mutations
 * after this point are at PDPT/PD/PT level — those pages are shared by
 * pointer across every PML4 and stay coherent automatically.
 */
static void prepopulate_kernel_pml4(void)
{
	for (unsigned i = 256; i < 512; i++) {
		paddr_t pa;

		if (kernel_pml4[i] & PTE_PRESENT) {
			continue;
		}
		pa = pmm_alloc_pages(0);
		if (!pa) {
			panic("paging: out of memory pre-populating kernel "
			      "PML4 slot %u",
			      i);
		}
		memset(phys_to_virt(pa), 0, PAGE_SIZE);
		kernel_pml4[i] = pa | PTE_PRESENT | PTE_WRITE;
	}
}

void paging_init(uint64_t hhdm)
{
	hhdm_offset = hhdm;
	kernel_pml4_phys = read_cr3() & 0x000FFFFFFFFFF000ull;
	kernel_pml4 = phys_to_virt(kernel_pml4_phys);

	enforce_nx_on_hhdm();
	enforce_nx_on_kernel_image();
	prepopulate_kernel_pml4();

	pr_info("paging: kernel PML4 at phys 0x%lx\n",
		(unsigned long)kernel_pml4_phys);
}

uint64_t *paging_kernel_pml4(void) { return kernel_pml4; }

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
		return NULL; /* mapping conflict at higher level */
	}
	return phys_to_virt(e & PTE_ADDR_MASK);
}

static unsigned pml4_idx(vaddr_t v) { return (v >> 39) & 0x1FF; }
static unsigned pdpt_idx(vaddr_t v) { return (v >> 30) & 0x1FF; }
static unsigned pd_idx(vaddr_t v) { return (v >> 21) & 0x1FF; }
static unsigned pt_idx(vaddr_t v) { return (v >> 12) & 0x1FF; }

/* ------------------------------------------------------------------------- */
/* Map / Unmap / Protect                                                      */
/* ------------------------------------------------------------------------- */

int paging_map(struct addr_space *space, vaddr_t virt, paddr_t phys,
	       size_t pages, uint64_t flags)
{
	uint64_t *pml4 = (space && space->pml4) ? space->pml4 : kernel_pml4;
	uint64_t intermediate_flags = (flags & PTE_USER) ? PTE_USER : 0;

	for (size_t i = 0; i < pages; i++) {
		vaddr_t v = virt + i * PAGE_SIZE;
		paddr_t p = phys + i * PAGE_SIZE;

		uint64_t *pdpt =
		    table_for(pml4, pml4_idx(v), true, intermediate_flags);
		if (!pdpt) {
			return -ENOMEM;
		}
		uint64_t *pd =
		    table_for(pdpt, pdpt_idx(v), true, intermediate_flags);
		if (!pd) {
			return -ENOMEM;
		}
		uint64_t *pt =
		    table_for(pd, pd_idx(v), true, intermediate_flags);
		if (!pt) {
			return -ENOMEM;
		}

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
		if (!pdpt) {
			continue;
		}
		uint64_t *pd = table_for(pdpt, pdpt_idx(v), false, 0);
		if (!pd) {
			continue;
		}
		uint64_t *pt = table_for(pd, pd_idx(v), false, 0);
		if (!pt) {
			continue;
		}

		uint64_t e = pt[pt_idx(v)];
		if (e & PTE_PRESENT) {
			if (e & PTE_USER) {
				paddr_t pa = e & PTE_ADDR_MASK;

				if (pa != mm_zero_page) {
					pmm_put_user_page(pa);
				}
			}
			pt[pt_idx(v)] = 0;
			paging_invlpg(v);
		}
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
		if (!pdpt) {
			return -ENOENT;
		}
		uint64_t *pd = table_for(pdpt, pdpt_idx(v), false, 0);
		if (!pd) {
			return -ENOENT;
		}
		uint64_t *pt = table_for(pd, pd_idx(v), false, 0);
		if (!pt) {
			return -ENOENT;
		}

		uint64_t e = pt[pt_idx(v)];
		if (!(e & PTE_PRESENT)) {
			return -ENOENT;
		}

		pt[pt_idx(v)] = (e & PTE_ADDR_MASK) | PTE_PRESENT | new_flags;
		paging_invlpg(v);
	}
	return 0;
}

int paging_get_flags(struct addr_space *space, vaddr_t virt,
		     uint64_t *out_flags)
{
	uint64_t *pml4 = (space && space->pml4) ? space->pml4 : kernel_pml4;
	uint64_t *pdpt;
	uint64_t *pd;
	uint64_t *pt;
	uint64_t e;

	if (!out_flags) {
		return -EINVAL;
	}

	pdpt = table_for(pml4, pml4_idx(virt), false, 0);
	if (!pdpt) {
		return -ENOENT;
	}

	e = pdpt[pdpt_idx(virt)];
	if (!(e & PTE_PRESENT)) {
		return -ENOENT;
	}
	if (e & PTE_HUGE) {
		*out_flags = e;
		return 0;
	}

	pd = table_for(pdpt, pdpt_idx(virt), false, 0);
	if (!pd) {
		return -ENOENT;
	}

	e = pd[pd_idx(virt)];
	if (!(e & PTE_PRESENT)) {
		return -ENOENT;
	}
	if (e & PTE_HUGE) {
		*out_flags = e;
		return 0;
	}

	pt = table_for(pd, pd_idx(virt), false, 0);
	if (!pt) {
		return -ENOENT;
	}

	e = pt[pt_idx(virt)];
	if (!(e & PTE_PRESENT)) {
		return -ENOENT;
	}

	*out_flags = e;
	return 0;
}

void paging_clone_kernel_half(uint64_t *new_pml4)
{
	for (unsigned i = 256; i < 512; i++) {
		new_pml4[i] = kernel_pml4[i];
	}
}

/*
 * Drop user-page references for every 4 KiB sub-page covered by a
 * single huge user PTE. Used when destroying an address space that
 * holds 2 MiB or 1 GiB user mappings. The contract is the same as
 * for 4 KiB user pages: each sub-page must have been individually
 * refcounted via pmm_alloc_user_page() (or be the global zero page).
 *
 * If a sub-page's refcount is already zero we treat it as
 * already-freed and skip — this leaves a tripwire for any caller
 * that produced a huge user PTE without setting per-sub-page
 * refcounts (pmm_put_user_page itself panics on refcount underflow,
 * so the bug surfaces loudly rather than silently corrupting
 * memory). v0.0.3 has no path that constructs huge user PTEs; the
 * machinery exists so adding one later does not silently leak.
 */
static void put_huge_user_range(paddr_t base, size_t span)
{
	for (size_t off = 0; off < span; off += PAGE_SIZE) {
		paddr_t pa = base + off;

		if (pa == mm_zero_page) {
			continue;
		}
		if (pmm_user_refcount(pa) == 0) {
			continue;
		}
		pmm_put_user_page(pa);
	}
}

void paging_destroy_user_half(uint64_t *pml4)
{
	if (!pml4 || pml4 == kernel_pml4) {
		return;
	}

	for (unsigned i4 = 0; i4 < 256; i4++) {
		uint64_t e4 = pml4[i4];
		if (!(e4 & PTE_PRESENT)) {
			continue;
		}

		uint64_t *pdpt = phys_to_virt(e4 & PTE_ADDR_MASK);
		for (unsigned i3 = 0; i3 < 512; i3++) {
			uint64_t e3 = pdpt[i3];
			if (!(e3 & PTE_PRESENT)) {
				continue;
			}
			if (e3 & PTE_HUGE) {
				/*
				 * 1 GiB huge user mapping. Drop the user
				 * reference on every 4 KiB sub-page; the
				 * physical 1 GiB block is freed implicitly
				 * once each sub-page's refcount falls to 0
				 * (pmm_put_user_page returns each frame to
				 * the buddy at order 0; coalescing rebuilds
				 * larger blocks). 1 GiB exceeds PMM_MAX_ORDER
				 * so we cannot free as a single block.
				 */
				put_huge_user_range(e3 & PTE_ADDR_MASK,
						    1ull << 30);
				continue;
			}

			uint64_t *pd = phys_to_virt(e3 & PTE_ADDR_MASK);
			for (unsigned i2 = 0; i2 < 512; i2++) {
				uint64_t e2 = pd[i2];
				if (!(e2 & PTE_PRESENT)) {
					continue;
				}
				if (e2 & PTE_HUGE) {
					/*
					 * 2 MiB huge user mapping. Drop
					 * the user reference on every
					 * 4 KiB sub-page; the previous
					 * implementation called
					 * pmm_free_pages(..., 9) which
					 * bypassed refcount tracking and
					 * would double-free a frame that
					 * was still shared with a child
					 * after CoW clone. The
					 * sub-page-iteration form is
					 * uniform with the 4 KiB and
					 * 1 GiB arms.
					 */
					put_huge_user_range(e2 & PTE_ADDR_MASK,
							    PAGE_HUGE_SIZE);
					continue;
				}

				uint64_t *pt = phys_to_virt(e2 & PTE_ADDR_MASK);
				for (unsigned i1 = 0; i1 < 512; i1++) {
					uint64_t e1 = pt[i1];
					if (e1 & PTE_PRESENT) {
						paddr_t pa = e1 & PTE_ADDR_MASK;

						if (pa != mm_zero_page) {
							pmm_put_user_page(pa);
						}
					}
				}
				pmm_free_pages(e2 & PTE_ADDR_MASK, 0);
			}
			pmm_free_pages(e3 & PTE_ADDR_MASK, 0);
		}
		pmm_free_pages(e4 & PTE_ADDR_MASK, 0);
		pml4[i4] = 0;
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
	paddr_t end = (base + len + PAGE_MASK) & ~(paddr_t)PAGE_MASK;

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
			pt[i1] = (pa & PTE_ADDR_MASK) | PTE_PRESENT |
				 PTE_WRITE | PTE_NX;
			paging_invlpg(va);
		}
	}
	return 0;
}
