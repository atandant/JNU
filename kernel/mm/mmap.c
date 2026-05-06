/*
 * kernel/mm/mmap.c — mmap / munmap / mprotect implementation.
 *
 * v0.0.3 §2.3–§2.4: anonymous private mappings only.  File-backed
 * and MAP_SHARED land in v0.0.4.  PTEs are NOT installed at mmap
 * time; the #PF handler resolves them lazily (§2.5).
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/kmalloc.h>
#include <jnu/mman.h>
#include <jnu/paging.h>
#include <jnu/pmm.h>
#include <jnu/process.h>
#include <jnu/sched.h>
#include <jnu/types.h>
#include <jnu/vma.h>
#include <jnu/vmm.h>

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

static uint32_t prot_to_vma(uint32_t prot)
{
	uint32_t flags = VMA_USER;

	if (prot & PROT_READ) {
		flags |= VMA_READ;
	}
	if (prot & PROT_WRITE) {
		flags |= VMA_WRITE;
	}
	if (prot & PROT_EXEC) {
		flags |= VMA_EXEC;
	}
	return flags;
}

static uint64_t vma_flags_to_pte(uint32_t vf)
{
	uint64_t pte = PTE_USER;

	if (vf & VMA_WRITE) {
		pte |= PTE_WRITE;
	}
	if (!(vf & VMA_EXEC)) {
		pte |= PTE_NX;
	}
	return pte;
}

static vaddr_t page_align_up(vaddr_t v)
{
	return (v + PAGE_MASK) & ~(vaddr_t)PAGE_MASK;
}

static struct addr_space *current_user_space(void)
{
	struct task *t = sched_current();

	if (!t || !t->process) {
		return NULL;
	}
	return t->process->space;
}

/* ------------------------------------------------------------------------- */
/* vmm_map_anonymous — v0.0.3 §2.8                                           */
/* ------------------------------------------------------------------------- */

int vmm_map_anonymous(struct addr_space *space, vaddr_t addr, size_t length,
		      uint32_t prot, uint32_t flags, vaddr_t *addr_out)
{
	struct vma *v;
	vaddr_t chosen;
	size_t aligned_len;
	uint32_t vma_flags;
	uint32_t known_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;
	int err;

	if (!space) {
		return -EINVAL;
	}
	if (length == 0) {
		return -EINVAL;
	}

	aligned_len = (size_t)page_align_up((vaddr_t)length);
	if (aligned_len == 0) {
		return -EINVAL; /* overflow */
	}

	/* v0.0.3: only MAP_PRIVATE | MAP_ANONYMOUS (± MAP_FIXED). */
	if (flags & ~known_flags) {
		return -EINVAL;
	}
	if (!(flags & MAP_PRIVATE) || !(flags & MAP_ANONYMOUS)) {
		return -EINVAL;
	}

	/* W^X enforcement (§2.3). */
	if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) {
		return -EINVAL;
	}

	vma_flags = prot_to_vma(prot);

	if (flags & MAP_FIXED) {
		if (addr & PAGE_MASK) {
			return -EINVAL;
		}
		if (addr < PAGE_SIZE || addr >= USER_TOP) {
			return -EINVAL;
		}
		if (addr + aligned_len < addr ||
		    addr + aligned_len > USER_TOP) {
			return -EINVAL;
		}
		chosen = addr;
		err = vma_remove_range(&space->vmas, space, chosen,
				       chosen + aligned_len);
		if (err) {
			return err;
		}
	} else {
		chosen = vma_find_gap_top_down(&space->vmas, aligned_len,
					       space->mmap_base);
		if (chosen == 0) {
			return -ENOMEM;
		}
	}

	/* Allocate and insert the VMA.  No PTEs — lazy fill on fault. */
	v = kzalloc(sizeof(*v));
	if (!v) {
		return -ENOMEM;
	}

	v->start = chosen;
	v->end = chosen + aligned_len;
	v->flags = vma_flags;

	err = vma_insert(&space->vmas, v);
	if (err) {
		kfree(v);
		return err;
	}

	if (addr_out) {
		*addr_out = chosen;
	}
	return 0;
}

/* ------------------------------------------------------------------------- */
/* sys_mmap                                                                   */
/* ------------------------------------------------------------------------- */

int64_t sys_mmap(uint64_t addr, uint64_t length, int prot, int flags, int fd,
		 int64_t offset)
{
	struct addr_space *space;
	vaddr_t result;
	int err;

	if (fd != -1 || offset != 0) {
		return -EINVAL;
	}

	space = current_user_space();
	if (!space) {
		return -EINVAL;
	}

	err = vmm_map_anonymous(space, (vaddr_t)addr, (size_t)length,
				(uint32_t)prot, (uint32_t)flags, &result);
	if (err) {
		return (int64_t)err;
	}

	return (int64_t)result;
}

/* ------------------------------------------------------------------------- */
/* sys_munmap                                                                 */
/* ------------------------------------------------------------------------- */

int64_t sys_munmap(uint64_t addr, uint64_t length)
{
	struct addr_space *space;
	size_t aligned_len;
	vaddr_t start;
	vaddr_t end;

	if (addr & PAGE_MASK) {
		return -EINVAL;
	}

	space = current_user_space();
	if (!space) {
		return -EINVAL;
	}

	aligned_len = (size_t)page_align_up((vaddr_t)length);
	if (aligned_len == 0) {
		return -EINVAL;
	}

	start = (vaddr_t)addr;
	end = start + aligned_len;
	if (end < start || end > USER_TOP) {
		return -EINVAL;
	}

	return (int64_t)vma_remove_range(&space->vmas, space, start, end);
}

/* ------------------------------------------------------------------------- */
/* sys_mprotect                                                               */
/* ------------------------------------------------------------------------- */

int64_t sys_mprotect(uint64_t addr, uint64_t length, int prot)
{
	struct addr_space *space;
	size_t aligned_len;
	vaddr_t start;
	vaddr_t end;
	struct vma *v;
	struct vma *next;
	uint32_t new_vma_flags;

	if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) {
		return -EINVAL;
	}
	if (addr & PAGE_MASK) {
		return -EINVAL;
	}

	space = current_user_space();
	if (!space) {
		return -EINVAL;
	}

	aligned_len = (size_t)page_align_up((vaddr_t)length);
	if (aligned_len == 0) {
		return -EINVAL;
	}

	start = (vaddr_t)addr;
	end = start + aligned_len;
	if (end < start || end > USER_TOP) {
		return -EINVAL;
	}

	v = vma_find(&space->vmas, start);
	if (v && v->start < start) {
		struct vma *upper;
		int err = vma_split_at(&space->vmas, v, start, &upper);

		if (err) {
			return (int64_t)err;
		}
	}

	v = vma_find(&space->vmas, end - 1);
	if (v && v->end > end) {
		struct vma *upper;
		int err = vma_split_at(&space->vmas, v, end, &upper);

		if (err) {
			return (int64_t)err;
		}
	}

	new_vma_flags = prot_to_vma((uint32_t)prot);

	v = vma_find(&space->vmas, start);
	if (!v) {
		v = vma_first(&space->vmas);
		while (v && v->end <= start) {
			v = vma_next(v);
		}
	}

	while (v && v->start < end) {
		uint32_t old_vma_flags = v->flags;
		next = vma_next(v);

		if (v->start >= start && v->end <= end) {
			v->flags = new_vma_flags;

			for (vaddr_t va = v->start; va < v->end;
			     va += PAGE_SIZE) {
				uint64_t pte;
				paddr_t pa;
				uint64_t new_pte;
				int err;

				err = paging_get_flags(space, va, &pte);
				if (err == -ENOENT) {
					continue;
				}
				if (err) {
					/*
					 * Restore the VMA's flags so the
					 * tree and the partially-updated
					 * page tables are at least
					 * consistent on the entries we
					 * have not touched yet. We cannot
					 * cleanly roll back the PTEs we
					 * already changed without a second
					 * walk; surfacing the error is the
					 * best the caller can do.
					 */
					v->flags = old_vma_flags;
					return (int64_t)err;
				}

				pa = pte & PTE_ADDR_MASK;
				new_pte = vma_flags_to_pte(new_vma_flags);

				/*
				 * CoW interaction (§2.4): do not grant
				 * PTE_WRITE on shared pages.  The CoW
				 * fault path resolves on first write.
				 */
				if (new_pte & PTE_WRITE) {
					if (pa == mm_zero_page ||
					    pmm_user_refcount(pa) > 1) {
						new_pte &= ~PTE_WRITE;
					}
				}

				err = paging_protect(space, va, 1, new_pte);
				if (err) {
					/*
					 * paging_protect's failure modes
					 * are -ENOENT (PTE walk hit a
					 * missing intermediate level) or
					 * -EINVAL. Restore the VMA flags
					 * and propagate; the tree is now
					 * consistent with itself but PTEs
					 * earlier in this VMA may already
					 * carry the new protection. Linux
					 * leaves the partial state in
					 * place too — the syscall ABI
					 * permits it.
					 */
					v->flags = old_vma_flags;
					return (int64_t)err;
				}
				paging_invlpg(va);
			}
		}

		v = next;
	}

	return 0;
}
