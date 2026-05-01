/*
 * kernel/mm/pmm.c — Physical memory manager: buddy allocator.
 *
 * 11 buddy orders (4 KiB → 4 MiB), zoned: ZONE_DMA covers the first
 * 16 MiB, ZONE_NORMAL covers the rest. Free lists are intrusive; we
 * allocate one `struct pmm_pfn` for every page frame in the machine,
 * indexed by PFN. Each entry holds:
 *
 *   - free flag
 *   - order at which the buddy is currently registered
 *   - next/prev links for the per-order free list
 *
 * The metadata array itself is carved out of the first sufficiently
 * large USABLE region in the Limine memory map. We then walk every
 * USABLE region and fold its pages into the free lists at the highest
 * natural order each span supports.
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

#include <limine.h>

/* ------------------------------------------------------------------------- */
/* Page-frame metadata                                                        */
/* ------------------------------------------------------------------------- */

#define PFN_FREE (1u << 0)

struct pmm_pfn {
	uint32_t flags;
	uint32_t order;
	uint64_t next; /* PFN of next on free list, or NO_LINK */
	uint64_t prev;
};

#define NO_LINK ((uint64_t)-1)

static struct pmm_pfn *pfn_table;
static uint64_t pfn_count;
static uint64_t hhdm_off;

static uint64_t free_list_head[PMM_ZONE_NR][PMM_MAX_ORDER];

static struct pmm_stats stats;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

static inline paddr_t pfn_to_pa(uint64_t pfn) { return pfn << 12; }
static inline uint64_t pa_to_pfn(paddr_t pa) { return pa >> 12; }

static enum pmm_zone zone_of(paddr_t pa)
{
	return (pa < (16ull * 1024 * 1024)) ? PMM_ZONE_DMA : PMM_ZONE_NORMAL;
}

static void list_remove(uint64_t pfn)
{
	struct pmm_pfn *e = &pfn_table[pfn];
	struct pmm_pfn *p = (e->prev == NO_LINK) ? NULL : &pfn_table[e->prev];
	struct pmm_pfn *n = (e->next == NO_LINK) ? NULL : &pfn_table[e->next];

	if (p) {
		p->next = e->next;
	} else {
		paddr_t pa = pfn_to_pa(pfn);
		free_list_head[zone_of(pa)][e->order] = e->next;
	}
	if (n) {
		n->prev = e->prev;
	}

	e->next = NO_LINK;
	e->prev = NO_LINK;
}

static void list_push(enum pmm_zone z, int order, uint64_t pfn)
{
	struct pmm_pfn *e = &pfn_table[pfn];
	uint64_t head = free_list_head[z][order];

	e->prev = NO_LINK;
	e->next = head;
	e->order = (uint32_t)order;
	e->flags |= PFN_FREE;

	if (head != NO_LINK) {
		pfn_table[head].prev = pfn;
	}
	free_list_head[z][order] = pfn;
}

static uint64_t list_pop(enum pmm_zone z, int order)
{
	uint64_t head = free_list_head[z][order];
	if (head == NO_LINK) {
		return NO_LINK;
	}
	list_remove(head);
	pfn_table[head].flags &= ~PFN_FREE;
	return head;
}

static uint64_t buddy_pfn(uint64_t pfn, int order)
{
	return pfn ^ (1ull << order);
}

/* ------------------------------------------------------------------------- */
/* Free / alloc                                                               */
/* ------------------------------------------------------------------------- */

static void pmm_register_block(uint64_t pfn, int order)
{
	enum pmm_zone z = zone_of(pfn_to_pa(pfn));
	list_push(z, order, pfn);
	stats.free_pages += (1ull << order);
	stats.free_by_order[order]++;
	stats.zone_free[z] += (1ull << order);
}

static void pmm_register_range(uint64_t start_pa, uint64_t end_pa)
{
	uint64_t start =
	    pa_to_pfn((start_pa + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
	uint64_t end = pa_to_pfn(end_pa & ~(PAGE_SIZE - 1));

	while (start < end) {
		int order = 0;
		while (order + 1 < PMM_MAX_ORDER &&
		       (start & ((1ull << (order + 1)) - 1)) == 0 &&
		       start + (1ull << (order + 1)) <= end) {
			order++;
		}
		pmm_register_block(start, order);
		start += (1ull << order);
	}
}

paddr_t pmm_alloc_pages(int order)
{
	if (order < 0 || order >= PMM_MAX_ORDER) {
		return 0;
	}

	for (enum pmm_zone z = PMM_ZONE_NORMAL;; z--) {
		int o = order;
		while (o < PMM_MAX_ORDER && free_list_head[z][o] == NO_LINK) {
			o++;
		}
		if (o < PMM_MAX_ORDER) {
			uint64_t pfn = list_pop(z, o);
			stats.free_pages -= (1ull << o);
			stats.free_by_order[o]--;
			stats.zone_free[z] -= (1ull << o);

			while (o > order) {
				o--;
				uint64_t buddy = pfn + (1ull << o);
				list_push(z, o, buddy);
				stats.free_pages += (1ull << o);
				stats.free_by_order[o]++;
				stats.zone_free[z] += (1ull << o);
			}

			pfn_table[pfn].flags &= ~PFN_FREE;
			pfn_table[pfn].order = (uint32_t)order;
			return pfn_to_pa(pfn);
		}
		if (z == PMM_ZONE_DMA) {
			break;
		}
	}
	return 0;
}

paddr_t pmm_alloc_zeroed_pages(int order)
{
	paddr_t pa = pmm_alloc_pages(order);

	if (pa) {
		memset(phys_to_virt(pa), 0, PMM_ORDER_SIZE(order));
	}

	return pa;
}

paddr_t pmm_alloc_user_page(void) { return pmm_alloc_zeroed_pages(0); }

paddr_t pmm_alloc_dma(int order)
{
	int o = order;
	while (o < PMM_MAX_ORDER &&
	       free_list_head[PMM_ZONE_DMA][o] == NO_LINK) {
		o++;
	}
	if (o >= PMM_MAX_ORDER) {
		return 0;
	}
	uint64_t pfn = list_pop(PMM_ZONE_DMA, o);
	stats.free_pages -= (1ull << o);
	stats.free_by_order[o]--;
	stats.zone_free[PMM_ZONE_DMA] -= (1ull << o);

	while (o > order) {
		o--;
		uint64_t buddy = pfn + (1ull << o);
		list_push(PMM_ZONE_DMA, o, buddy);
		stats.free_pages += (1ull << o);
		stats.free_by_order[o]++;
		stats.zone_free[PMM_ZONE_DMA] += (1ull << o);
	}

	pfn_table[pfn].flags &= ~PFN_FREE;
	pfn_table[pfn].order = (uint32_t)order;
	return pfn_to_pa(pfn);
}

void pmm_free_pages(paddr_t pa, int order)
{
	if (order < 0 || order >= PMM_MAX_ORDER) {
		panic("pmm_free_pages: bad order %d", order);
	}
	if (pa & ((1ull << (12 + order)) - 1)) {
		panic("pmm_free_pages: misaligned 0x%lx order %d",
		      (unsigned long)pa, order);
	}

	uint64_t pfn = pa_to_pfn(pa);
	if (pfn >= pfn_count) {
		panic("pmm_free_pages: pfn out of range");
	}
	if (pfn_table[pfn].flags & PFN_FREE) {
		panic("pmm_free_pages: double-free at 0x%lx",
		      (unsigned long)pa);
	}

	enum pmm_zone z = zone_of(pa);

	while (order + 1 < PMM_MAX_ORDER) {
		uint64_t bpfn = buddy_pfn(pfn, order);
		if (bpfn >= pfn_count) {
			break;
		}
		struct pmm_pfn *bp = &pfn_table[bpfn];
		if (!(bp->flags & PFN_FREE) || bp->order != (uint32_t)order) {
			break;
		}
		if (zone_of(pfn_to_pa(bpfn)) != z) {
			break;
		}

		list_remove(bpfn);
		stats.free_pages -= (1ull << order);
		stats.free_by_order[order]--;
		stats.zone_free[z] -= (1ull << order);

		bp->flags &= ~PFN_FREE;

		if (bpfn < pfn) {
			pfn = bpfn;
		}
		order++;
	}

	list_push(z, order, pfn);
	stats.free_pages += (1ull << order);
	stats.free_by_order[order]++;
	stats.zone_free[z] += (1ull << order);
}

void pmm_get_stats(struct pmm_stats *out) { *out = stats; }

void pmm_dump(void)
{
	pr_info("pmm: total=%lu free=%lu pages (%lu MiB free)\n",
		(unsigned long)stats.total_pages,
		(unsigned long)stats.free_pages,
		(unsigned long)((stats.free_pages * PAGE_SIZE) >> 20));
	pr_info("pmm: dma free=%lu, normal free=%lu\n",
		(unsigned long)stats.zone_free[PMM_ZONE_DMA],
		(unsigned long)stats.zone_free[PMM_ZONE_NORMAL]);
}

/* ------------------------------------------------------------------------- */
/* Init                                                                       */
/* ------------------------------------------------------------------------- */

void pmm_init(const struct limine_memmap_response *mm, uint64_t hhdm_offset)
{
	hhdm_off = hhdm_offset;

	for (int z = 0; z < PMM_ZONE_NR; z++) {
		for (int o = 0; o < PMM_MAX_ORDER; o++) {
			free_list_head[z][o] = NO_LINK;
		}
	}

	memset(&stats, 0, sizeof(stats));

	if (!mm) {
		panic("pmm: no memory map from Limine");
	}

	/* Find highest USABLE end. */
	uint64_t highest_end = 0;
	for (uint64_t i = 0; i < mm->entry_count; i++) {
		const struct limine_memmap_entry *e = mm->entries[i];
		if (e->type != LIMINE_MEMMAP_USABLE) {
			continue;
		}
		uint64_t end = e->base + e->length;
		if (end > highest_end) {
			highest_end = end;
		}
	}
	if (highest_end == 0) {
		panic("pmm: no usable memory");
	}

	pfn_count = highest_end / PAGE_SIZE;
	uint64_t meta_bytes = pfn_count * sizeof(struct pmm_pfn);
	uint64_t meta_pages = (meta_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

	/* Carve metadata from the first USABLE region big enough. */
	uint64_t meta_pa = 0;
	for (uint64_t i = 0; i < mm->entry_count; i++) {
		struct limine_memmap_entry *e = mm->entries[i];
		if (e->type != LIMINE_MEMMAP_USABLE) {
			continue;
		}
		if (e->length >= meta_pages * PAGE_SIZE) {
			meta_pa = e->base;
			e->base += meta_pages * PAGE_SIZE;
			e->length -= meta_pages * PAGE_SIZE;
			break;
		}
	}
	if (meta_pa == 0) {
		panic("pmm: cannot place metadata array");
	}

	pfn_table = (struct pmm_pfn *)(uintptr_t)(meta_pa + hhdm_off);
	memset(pfn_table, 0, meta_bytes);
	for (uint64_t i = 0; i < pfn_count; i++) {
		pfn_table[i].next = NO_LINK;
		pfn_table[i].prev = NO_LINK;
	}

	/* Now feed every remaining USABLE byte into the buddy. */
	uint64_t total_free_pages = 0;
	for (uint64_t i = 0; i < mm->entry_count; i++) {
		const struct limine_memmap_entry *e = mm->entries[i];
		if (e->type != LIMINE_MEMMAP_USABLE) {
			continue;
		}
		pmm_register_range(e->base, e->base + e->length);
		total_free_pages += e->length / PAGE_SIZE;

		enum pmm_zone z_lo = zone_of(e->base);
		enum pmm_zone z_hi = zone_of(e->base + e->length - 1);
		(void)z_lo;
		(void)z_hi;
	}

	stats.total_pages = pfn_count;
	stats.zone_total[PMM_ZONE_DMA] =
	    (highest_end < 16ull * 1024 * 1024 ? highest_end
					       : 16ull * 1024 * 1024) /
	    PAGE_SIZE;
	stats.zone_total[PMM_ZONE_NORMAL] =
	    (highest_end > 16ull * 1024 * 1024)
		? (highest_end - 16ull * 1024 * 1024) / PAGE_SIZE
		: 0;
	(void)total_free_pages;

	pmm_dump();
}

/* ------------------------------------------------------------------------- */
/* Selftest                                                                   */
/* ------------------------------------------------------------------------- */

#define PMM_TEST_PAGES 1024
#define PMM_TEST_ORDER3 64

int pmm_selftest(void)
{
	static paddr_t pages[PMM_TEST_PAGES];
	static paddr_t blocks[PMM_TEST_ORDER3];

	struct pmm_stats before;
	pmm_get_stats(&before);

	for (size_t i = 0; i < PMM_TEST_PAGES; i++) {
		pages[i] = pmm_alloc_pages(0);
		if (!pages[i]) {
			pr_err("pmm: alloc page %lu failed\n",
			       (unsigned long)i);
			return -ENOMEM;
		}
	}
	for (size_t i = 0; i < PMM_TEST_ORDER3; i++) {
		blocks[i] = pmm_alloc_pages(3);
		if (!blocks[i]) {
			pr_err("pmm: alloc order-3 block %lu failed\n",
			       (unsigned long)i);
			return -ENOMEM;
		}
	}

	/* Free in scrambled order so coalescing runs through every path. */
	uint32_t s = 0xDEADBEEFu;
	for (size_t i = 0; i < PMM_TEST_PAGES; i++) {
		s = s * 1103515245u + 12345u;
		size_t j = ((s >> 16) % (PMM_TEST_PAGES - i)) + i;
		paddr_t tmp = pages[i];
		pages[i] = pages[j];
		pages[j] = tmp;
	}
	for (size_t i = 0; i < PMM_TEST_PAGES; i++) {
		pmm_free_pages(pages[i], 0);
	}
	for (size_t i = 0; i < PMM_TEST_ORDER3; i++) {
		pmm_free_pages(blocks[i], 3);
	}

	struct pmm_stats after;
	pmm_get_stats(&after);

	if (after.free_pages != before.free_pages) {
		pr_err("pmm: leak: %lu before, %lu after\n",
		       (unsigned long)before.free_pages,
		       (unsigned long)after.free_pages);
		return -EINVAL;
	}

	return 0;
}
