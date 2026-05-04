/*
 * kernel/mm/clone_space.c — CoW address space cloning.
 *
 * v0.0.2.2's CoW clone.  Walks the source VMA tree, allocates a
 * matching VMA descriptor in the destination, then for every present
 * 4 KiB user PTE in the source:
 *
 *   1. Strips PTE_WRITE from the source PTE.
 *   2. Installs the same physical address in the destination PTE,
 *      with PTE_WRITE cleared and the same PTE_USER/PTE_NX bits.
 *   3. Calls pmm_get_user_page(pa) once per added reference.
 *   4. Invalidates the TLB for the source VA so the source side
 *      starts taking write faults too.
 *
 * The destination VMA preserves the logical writability (VMA_WRITE).
 * The PTE is the source of truth for "is this page currently CoW-
 * shared"; the VMA is the source of truth for "may this address
 * ever be written".
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
 * Bits we propagate from the source PTE into the destination PTE.
 * PTE_WRITE is deliberately included in the mask so we can clear
 * it below; we never set it in the destination.
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
		paddr_t pa;
		uint64_t dst_flags;

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
			 * v0.0.2.2 user mappings are 4 KiB only. A huge
			 * user PTE means the loader or VMM produced
			 * something this clone path is not prepared for.
			 */
			pr_err("clone_space: huge user PTE at 0x%lx\n",
			       (unsigned long)va);
			return -EINVAL;
		}

		pa = src_pte & PTE_ADDR_MASK;

		/*
		 * Strip PTE_WRITE from the source PTE so the source
		 * side starts taking write faults on this page too.
		 * Pages that were already RO need no PTE change beyond
		 * the refcount bump below.
		 */
		if (src_pte & PTE_WRITE) {
			err = paging_protect(src, va, 1,
					     (src_pte & CLONE_PTE_FLAGS_MASK) &
						 ~PTE_WRITE);
			if (err) {
				return err;
			}
			paging_invlpg(va);
		}

		/*
		 * Install the same PA in the destination with PTE_WRITE
		 * cleared and the same PTE_USER/PTE_NX bits.
		 */
		dst_flags = (src_pte & CLONE_PTE_FLAGS_MASK) & ~PTE_WRITE;
		err = paging_map(dst, va, pa, 1, dst_flags);
		if (err) {
			return err;
		}

		/*
		 * Bump the refcount for the shared page.
		 *
		 * v0.0.3 §2.5: the zero page's refcount is 0 forever
		 * and must never be touched.  Skip the bump — the
		 * zero page is a shared singleton that is never freed.
		 */
		if (pa != mm_zero_page) {
			pmm_get_user_page(pa);
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
		pmm_put_user_page(pa);
		return err;
	}

	{
		struct vma *v = kzalloc(sizeof(*v));

		if (!v) {
			vmm_unmap(space, CLONE_TEST_VA, 1);
			pmm_put_user_page(pa);
			return -ENOMEM;
		}
		v->start = CLONE_TEST_VA;
		v->end = CLONE_TEST_VA + PAGE_SIZE;
		v->flags = VMA_READ | VMA_WRITE | VMA_USER;
		err = vma_insert(&space->vmas, v);
		if (err) {
			kfree(v);
			vmm_unmap(space, CLONE_TEST_VA, 1);
			pmm_put_user_page(pa);
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

	/* After CoW clone, both sides share the same physical frame. */
	if (read_test_value(dst) != CLONE_TEST_SENTINEL) {
		err = -EIO;
		goto fail_dst;
	}

	/*
	 * Verify the refcount: the page should now have refcount == 2
	 * (one from the source, one from the destination).
	 */
	{
		uint64_t src_pte;
		paddr_t pa;

		err = paging_get_flags(src, CLONE_TEST_VA, &src_pte);
		if (err) {
			goto fail_dst;
		}
		pa = src_pte & PTE_ADDR_MASK;

		if (pmm_user_refcount(pa) != 2) {
			pr_err("clone_space_selftest: expected refcount 2, "
			       "got %u\n",
			       (unsigned)pmm_user_refcount(pa));
			err = -EIO;
			goto fail_dst;
		}

		/* Verify PTE_WRITE was stripped from the source. */
		if (src_pte & PTE_WRITE) {
			pr_err("clone_space_selftest: source PTE still "
			       "writable after CoW clone\n");
			err = -EIO;
			goto fail_dst;
		}
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
