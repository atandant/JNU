/*
 * kernel/mm/clone_space.c — Deep-copy address space cloning.
 *
 * Phase 1 of v0.0.2.1's fork plumbing. Walks the source VMA tree,
 * allocates a matching VMA descriptor in the destination, then for
 * every present 4 KiB user PTE in the source allocates a fresh page,
 * memcpys the contents through the HHDM, and installs the new PTE
 * with the same USER/WRITE/NX protection bits.
 *
 * v0.0.2.1 deliberately does NOT introduce PMM page refcounts: every
 * page is duplicated. CoW is the v0.0.2.2 release per jnuspec021.md
 * §2.1. The selftest in this file exercises isolation in both
 * directions (mutating the parent must not change the child, and
 * vice versa).
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/compiler.h>
#include <jnu/errno.h>
#include <jnu/klog.h>
#include <jnu/kmalloc.h>
#include <jnu/paging.h>
#include <jnu/pmm.h>
#include <jnu/rbtree.h>
#include <jnu/string.h>
#include <jnu/vma.h>
#include <jnu/vmm.h>

/*
 * Bits we propagate from the source PTE into the destination PTE. We
 * deliberately drop ACCESSED, DIRTY, GLOBAL, and the PWT/PCD caching
 * bits: the child gets a clean slate. PTE_PRESENT is OR'd back in by
 * paging_map().
 */
#define CLONE_PTE_FLAGS_MASK (PTE_WRITE | PTE_USER | PTE_NX)

static struct vma *vma_from_node(struct rb_node *node)
{
	return (struct vma *)((uint8_t *)node -
			      __builtin_offsetof(struct vma, rb));
}

static int clone_one_vma(struct addr_space *src, struct addr_space *dst,
			 const struct vma *src_vma)
{
	struct vma *dst_vma;
	int err;

	dst_vma = kzalloc(sizeof(*dst_vma));
	if (!dst_vma) {
		return -ENOMEM;
	}
	dst_vma->start = src_vma->start;
	dst_vma->end = src_vma->end;
	dst_vma->flags = src_vma->flags;

	err = vma_insert(&dst->vmas, dst_vma);
	if (err) {
		kfree(dst_vma);
		return err;
	}

	for (vaddr_t va = src_vma->start; va < src_vma->end; va += PAGE_SIZE) {
		uint64_t src_pte;
		paddr_t src_pa;
		paddr_t dst_pa;
		uint64_t pte_flags;

		err = paging_get_flags(src, va, &src_pte);
		if (err == -ENOENT) {
			/* Sparse page within the VMA; legal, leave unmapped. */
			continue;
		}
		if (err) {
			return err;
		}
		if (src_pte & PTE_HUGE) {
			/*
			 * v0.0.2.1 user mappings are 4 KiB only. A huge
			 * user PTE means the loader or VMM produced
			 * something this clone path is not prepared for.
			 */
			pr_err("clone_space: huge user PTE at 0x%lx\n",
			       (unsigned long)va);
			return -EINVAL;
		}

		dst_pa = pmm_alloc_user_page();
		if (!dst_pa) {
			return -ENOMEM;
		}

		src_pa = src_pte & PTE_ADDR_MASK;
		memcpy(phys_to_virt(dst_pa), phys_to_virt(src_pa), PAGE_SIZE);

		pte_flags = src_pte & CLONE_PTE_FLAGS_MASK;
		err = paging_map(dst, va, dst_pa, 1, pte_flags);
		if (err) {
			pmm_free_pages(dst_pa, 0);
			return err;
		}
	}

	return 0;
}

int vmm_clone_space(struct addr_space *src, struct addr_space **out)
{
	struct addr_space *dst;
	struct rb_node *node;

	if (!src || !out) {
		return -EINVAL;
	}

	dst = vmm_create_space();
	if (!dst) {
		return -ENOMEM;
	}

	for (node = rb_first(&src->vmas); node; node = rb_next(node)) {
		struct vma *src_vma = vma_from_node(node);
		int err = clone_one_vma(src, dst, src_vma);

		if (err) {
			/*
			 * vmm_destroy_space walks the VMA tree, releases
			 * every present user PTE through
			 * paging_destroy_user_half, and frees the PML4.
			 * That undoes every page we successfully cloned
			 * above plus the partial vma we may have
			 * inserted.
			 */
			vmm_destroy_space(dst);
			return err;
		}
	}

	*out = dst;
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Selftest                                                                   */
/* ------------------------------------------------------------------------- */

#define CLONE_TEST_VA 0x0000000000401000ull
#define CLONE_TEST_SENTINEL 0x4A4E55434C4F4E45ull /* "JNUCLONE" */

static int populate_test_space(struct addr_space *space, uint64_t value)
{
	paddr_t pa;
	int err;

	pa = pmm_alloc_user_page();
	if (!pa) {
		return -ENOMEM;
	}
	*(volatile uint64_t *)phys_to_virt(pa) = value;

	err = vmm_map(space, CLONE_TEST_VA, pa, 1,
		      VMA_READ | VMA_WRITE | VMA_USER);
	if (err) {
		pmm_free_pages(pa, 0);
		return err;
	}

	{
		struct vma *v = kzalloc(sizeof(*v));

		if (!v) {
			vmm_unmap(space, CLONE_TEST_VA, 1);
			pmm_free_pages(pa, 0);
			return -ENOMEM;
		}
		v->start = CLONE_TEST_VA;
		v->end = CLONE_TEST_VA + PAGE_SIZE;
		v->flags = VMA_READ | VMA_WRITE | VMA_USER;
		err = vma_insert(&space->vmas, v);
		if (err) {
			kfree(v);
			vmm_unmap(space, CLONE_TEST_VA, 1);
			pmm_free_pages(pa, 0);
			return err;
		}
	}

	return 0;
}

static uint64_t read_test_value(struct addr_space *space)
{
	uint64_t pte;

	if (paging_get_flags(space, CLONE_TEST_VA, &pte) != 0) {
		return 0;
	}
	return *(volatile uint64_t *)phys_to_virt(pte & PTE_ADDR_MASK);
}

static int write_test_value(struct addr_space *space, uint64_t value)
{
	uint64_t pte;

	if (paging_get_flags(space, CLONE_TEST_VA, &pte) != 0) {
		return -ENOENT;
	}
	*(volatile uint64_t *)phys_to_virt(pte & PTE_ADDR_MASK) = value;
	return 0;
}

int clone_space_selftest(void)
{
	struct addr_space *src = NULL;
	struct addr_space *dst = NULL;
	struct pmm_stats before;
	struct pmm_stats after;
	int err;

	pmm_get_stats(&before);

	src = vmm_create_space();
	if (!src) {
		return -ENOMEM;
	}

	err = populate_test_space(src, CLONE_TEST_SENTINEL);
	if (err) {
		goto fail_src;
	}

	err = vmm_clone_space(src, &dst);
	if (err) {
		goto fail_src;
	}

	if (read_test_value(dst) != CLONE_TEST_SENTINEL) {
		err = -EIO;
		goto fail_dst;
	}

	/* Mutating the source must not affect the destination. */
	err = write_test_value(src, 0xDEADBEEFCAFEBABEull);
	if (err) {
		goto fail_dst;
	}
	if (read_test_value(dst) != CLONE_TEST_SENTINEL) {
		err = -EIO;
		goto fail_dst;
	}

	/* Mutating the destination must not affect the source. */
	err = write_test_value(dst, 0xFEEDFACEFEEDFACEull);
	if (err) {
		goto fail_dst;
	}
	if (read_test_value(src) != 0xDEADBEEFCAFEBABEull) {
		err = -EIO;
		goto fail_dst;
	}

	err = 0;

fail_dst:
	vmm_destroy_space(dst);
fail_src:
	vmm_destroy_space(src);

	pmm_get_stats(&after);
	if (err == 0 && after.free_pages != before.free_pages) {
		pr_err("clone_space_selftest: leaked %ld pages\n",
		       (long)(before.free_pages - after.free_pages));
		return -EIO;
	}
	return err;
}
