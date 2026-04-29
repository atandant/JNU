/*
 * include/jnu/pmm.h — Physical memory manager (buddy allocator).
 *
 * 11 buddy orders (4 KiB → 4 MiB), zoned: ZONE_DMA covers the first
 * 16 MiB, ZONE_NORMAL covers the rest. Backed by the Limine memory
 * map. PMM owns the page-frame metadata array; everything else asks
 * for pages and gets back physical addresses.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>

#define PMM_MAX_ORDER		11
#define PMM_ORDER_SIZE(o)	(1ull << (12 + (o)))

enum pmm_zone {
	PMM_ZONE_DMA	= 0,	/* < 16 MiB */
	PMM_ZONE_NORMAL	= 1,
	PMM_ZONE_NR
};

struct pmm_stats {
	uint64_t	total_pages;
	uint64_t	free_pages;
	uint64_t	free_by_order[PMM_MAX_ORDER];
	uint64_t	zone_total[PMM_ZONE_NR];
	uint64_t	zone_free[PMM_ZONE_NR];
};

struct limine_memmap_response;

/*
 * Bring up the PMM from the Limine memory map. Walks USABLE entries,
 * adds them to the buddy free lists at the highest natural order each
 * span supports.
 */
void pmm_init(const struct limine_memmap_response *mm, uint64_t hhdm_offset);

/*
 * Allocate 2^order contiguous pages. Returns the physical base address
 * or 0 on failure. The caller must use phys_to_virt() to access them.
 */
paddr_t pmm_alloc_pages(int order);

paddr_t pmm_alloc_zeroed_pages(int order);
paddr_t pmm_alloc_user_page(void);

paddr_t pmm_alloc_dma(int order);

void pmm_free_pages(paddr_t pa, int order);

void pmm_get_stats(struct pmm_stats *out);

void pmm_dump(void);

int pmm_selftest(void);
