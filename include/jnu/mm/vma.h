/*
 * include/jnu/mm/vma.h — Per-address-space VMA tree primitives.
 *
 * Operations on the rbtree of `struct vma` inside a `struct addr_space`.
 * The vma type itself is declared in <jnu/vmm.h>.
 *
 * v0.0.3 adds splitting, range removal, and gap-finding helpers
 * required by the mmap / munmap / mprotect syscalls.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/base/types.h>
#include <jnu/mm/vmm.h>

/*
 * Insert `vma` into the tree. Rejects overlap; returns -EEXIST if the
 * range already overlaps an existing VMA, 0 on success.
 */
int vma_insert(struct rb_root *root, struct vma *v);

/* Find the VMA containing `addr`, or NULL. O(log n). */
struct vma *vma_find(const struct rb_root *root, vaddr_t addr);

void vma_remove(struct rb_root *root, struct vma *v);

/* Iteration: first VMA (lowest start address) and successor. */
struct vma *vma_first(const struct rb_root *root);
struct vma *vma_next(const struct vma *v);

/*
 * Split `vma` at page-aligned `boundary` (must be strictly inside the
 * VMA's range).  After the call:
 *   - `vma` covers [vma->start, boundary)
 *   - *out  covers [boundary, original_end)
 *
 * Returns 0 / -ENOMEM / -EINVAL.
 */
int vma_split_at(struct rb_root *root, struct vma *vma, vaddr_t boundary,
		 struct vma **out);

/*
 * Remove every VMA fully inside [start, end). Partial-overlap VMAs at
 * the boundaries are split first. Freed VMA descriptors are kfree'd.
 * PTEs in the removed ranges are unmapped via paging_unmap if `space`
 * is non-NULL.
 *
 * Returns 0 / negative errno (allocation failure during split).
 */
int vma_remove_range(struct rb_root *root, struct addr_space *space,
		     vaddr_t start, vaddr_t end);

/*
 * Top-down first-fit gap finder.  Returns the base address of the
 * highest gap of at least `size` page-aligned bytes below `ceiling`,
 * or 0 if no gap large enough exists.  `ceiling` is typically
 * space->mmap_base (randomized per-process for ASLR).
 */
vaddr_t vma_find_gap_top_down(const struct rb_root *root, size_t size,
			      vaddr_t ceiling);
