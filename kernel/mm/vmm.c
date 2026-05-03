/*
 * kernel/mm/vmm.c — Address spaces and high-level virtual mapping.
 *
 * Wraps a PML4 plus a VMA tree per address space. v0.0.1 has exactly
 * one address space (the kernel one), so vmm_create_space and
 * vmm_destroy_space exist mostly for completeness — they are not yet
 * exercised at boot.
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
#include <jnu/types.h>
#include <jnu/vma.h>
#include <jnu/vmm.h>

static struct addr_space kernel_space;

static uint64_t vma_to_pte_flags(uint32_t f)
{
	uint64_t pte = 0;
	if (f & VMA_WRITE) {
		pte |= PTE_WRITE;
	}
	if (f & VMA_USER) {
		pte |= PTE_USER;
	}
	if (!(f & VMA_EXEC)) {
		pte |= PTE_NX;
	}
	return pte;
}

void vmm_init(void)
{
	kernel_space.pml4 = paging_kernel_pml4();
	kernel_space.pml4_phys = virt_to_phys(kernel_space.pml4);
	rb_init(&kernel_space.vmas);

	pr_info("vmm: kernel address space initialized\n");
}

struct addr_space *vmm_kernel_space(void) { return &kernel_space; }

struct addr_space *vmm_create_space(void)
{
	struct addr_space *space;
	paddr_t pml4_pa;

	space = kzalloc(sizeof(*space));
	if (!space) {
		return NULL;
	}

	pml4_pa = pmm_alloc_zeroed_pages(0);
	if (!pml4_pa) {
		kfree(space);
		return NULL;
	}

	space->pml4 = phys_to_virt(pml4_pa);
	space->pml4_phys = pml4_pa;
	rb_init(&space->vmas);
	paging_clone_kernel_half(space->pml4);
	return space;
}

void vmm_destroy_space(struct addr_space *space)
{
	struct rb_node *node;

	if (!space || space == &kernel_space) {
		return;
	}

	/*
	 * Free every VMA descriptor allocated by map_zeroed_user_pages().
	 * Without this walk, every process exit leaks all VMA nodes.
	 */
	while ((node = rb_first(&space->vmas)) != NULL) {
		struct vma *v =
		    (struct vma *)((uint8_t *)node -
				   __builtin_offsetof(struct vma, rb));
		rb_erase(&space->vmas, node);
		kfree(v);
	}

	paging_destroy_user_half(space->pml4);
	pmm_free_pages(space->pml4_phys, 0);
	kfree(space);
}

int vmm_map(struct addr_space *space, vaddr_t virt, paddr_t phys, size_t pages,
	    uint32_t flags)
{
	int err = paging_map(space, virt, phys, pages, vma_to_pte_flags(flags));
	if (err) {
		return err;
	}
	return 0;
}

int vmm_unmap(struct addr_space *space, vaddr_t virt, size_t pages)
{
	return paging_unmap(space, virt, pages);
}

int vmm_protect(struct addr_space *space, vaddr_t virt, size_t pages,
		uint32_t new_flags)
{
	return paging_protect(space, virt, pages, vma_to_pte_flags(new_flags));
}

void vmm_switch_to(struct addr_space *space)
{
	paddr_t cr3 = space ? space->pml4_phys : kernel_space.pml4_phys;
	__asm__ __volatile__("mov %0, %%cr3" ::"r"(cr3) : "memory");
}

/* ------------------------------------------------------------------------- */
/* CoW fault resolution                                                       */
/* ------------------------------------------------------------------------- */

int vmm_handle_cow_fault(struct addr_space *space, vaddr_t va)
{
	uint64_t old_pte;
	paddr_t old_pa;
	paddr_t new_pa;
	uint64_t new_flags;
	int err;

	err = paging_get_flags(space, va, &old_pte);
	if (err) {
		return err;
	}

	old_pa = old_pte & PTE_ADDR_MASK;

	/*
	 * Fast path: if the refcount has dropped to 1 (the other side
	 * already copied or exited), just re-enable PTE_WRITE — no
	 * allocation, no copy.
	 */
	if (pmm_user_refcount(old_pa) == 1) {
		err = paging_protect(space, va, 1,
				     (old_pte & (PTE_USER | PTE_NX)) |
					 PTE_WRITE);
		if (err) {
			return err;
		}
		paging_invlpg(va);
		return 0;
	}

	/*
	 * Slow path: allocate a private copy, memcpy the old contents,
	 * install the new PTE with PTE_WRITE, and drop the old ref.
	 */
	new_pa = pmm_alloc_user_page();
	if (!new_pa) {
		return -ENOMEM;
	}

	memcpy(phys_to_virt(new_pa), phys_to_virt(old_pa), PAGE_SIZE);

	new_flags = (old_pte & (PTE_USER | PTE_NX)) | PTE_WRITE;
	err = paging_map(space, va, new_pa, 1, new_flags);
	if (err) {
		goto fail_page;
	}

	pmm_put_user_page(old_pa);
	paging_invlpg(va);
	return 0;

fail_page:
	pmm_put_user_page(new_pa);
	return err;
}

/* ------------------------------------------------------------------------- */
/* Selftest                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * The vmalloc region begins at 0xFFFF_A000_0000_0000 (§2.3). For the
 * test we map two pages in there, write a sentinel, change protection
 * to RO + NX, then unmap. Catching the write fault would require an
 * exception-fixup table; the spec's harder selftest is recorded as a
 * Phase 3 task in the comments below.
 */
#define VMM_TEST_VA 0xFFFFA00000010000ull

int vmm_selftest(void)
{
	paddr_t pa = pmm_alloc_pages(0);
	if (!pa) {
		pr_err("vmm: selftest oom\n");
		return -ENOMEM;
	}

	int err =
	    vmm_map(&kernel_space, VMM_TEST_VA, pa, 1, VMA_READ | VMA_WRITE);
	if (err) {
		pmm_free_pages(pa, 0);
		return err;
	}

	volatile uint64_t *p = (volatile uint64_t *)VMM_TEST_VA;
	*p = 0xDEADBEEFCAFEBABEull;
	if (*p != 0xDEADBEEFCAFEBABEull) {
		pr_err("vmm: written value did not read back\n");
		vmm_unmap(&kernel_space, VMM_TEST_VA, 1);
		pmm_free_pages(pa, 0);
		return -EIO;
	}

	err = vmm_protect(&kernel_space, VMM_TEST_VA, 1, VMA_READ);
	if (err) {
		vmm_unmap(&kernel_space, VMM_TEST_VA, 1);
		pmm_free_pages(pa, 0);
		return err;
	}

	/* Read still works. */
	if (*p != 0xDEADBEEFCAFEBABEull) {
		pr_err("vmm: read after RO-reprotect failed\n");
		vmm_unmap(&kernel_space, VMM_TEST_VA, 1);
		pmm_free_pages(pa, 0);
		return -EIO;
	}

	vmm_unmap(&kernel_space, VMM_TEST_VA, 1);
	pmm_free_pages(pa, 0);
	return 0;
}
