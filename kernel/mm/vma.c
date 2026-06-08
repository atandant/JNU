/*
 * kernel/mm/vma.c — Per-address-space VMA tree.
 *
 * The actual rb-tree mechanics live in lib/rbtree.c.  This file orders
 * VMAs by start address and provides insert, lookup, removal, splitting,
 * range-removal, and gap-finding operations.
 *
 * v0.0.3 adds VMA splitting and gap-finding for mmap / munmap / mprotect.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/base/types.h>
#include <jnu/lib/rbtree.h>
#include <jnu/mm/kmalloc.h>
#include <jnu/mm/paging.h>
#include <jnu/mm/pmm.h>
#include <jnu/mm/vma.h>
#include <jnu/mm/vmm.h>
#include <uapi/jnu/errno.h>
#include <uapi/jnu/mman.h>

/* ------------------------------------------------------------------------- */
/* Container-of helper                                                        */
/* ------------------------------------------------------------------------- */

static struct vma *node_to_vma(struct rb_node *n)
{
	if (!n) {
		return NULL;
	}
	return (struct vma *)((uint8_t *)n -
			      __builtin_offsetof(struct vma, rb));
}

/* ------------------------------------------------------------------------- */
/* Core operations                                                            */
/* ------------------------------------------------------------------------- */

int vma_insert(struct rb_root *root, struct vma *v)
{
	struct rb_node **slot = &root->root;
	struct rb_node *parent = NULL;

	while (*slot) {
		parent = *slot;
		struct vma *cur = node_to_vma(*slot);

		if (v->end <= cur->start) {
			slot = &(*slot)->left;
		} else if (v->start >= cur->end) {
			slot = &(*slot)->right;
		} else {
			return -EEXIST;
		}
	}

	rb_link_node(&v->rb, parent, slot);
	rb_insert_color(root, &v->rb);
	return 0;
}

struct vma *vma_find(const struct rb_root *root, vaddr_t addr)
{
	struct rb_node *n = root->root;

	while (n) {
		struct vma *cur = node_to_vma(n);

		if (addr < cur->start) {
			n = n->left;
		} else if (addr >= cur->end) {
			n = n->right;
		} else {
			return cur;
		}
	}
	return NULL;
}

void vma_remove(struct rb_root *root, struct vma *v) { rb_erase(root, &v->rb); }

/* ------------------------------------------------------------------------- */
/* Iteration                                                                  */
/* ------------------------------------------------------------------------- */

struct vma *vma_first(const struct rb_root *root)
{
	return node_to_vma(rb_first(root));
}

struct vma *vma_next(const struct vma *v)
{
	return node_to_vma(rb_next(&v->rb));
}

/* ------------------------------------------------------------------------- */
/* Splitting — v0.0.3 §4.2                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Split `vma` at page-aligned `boundary`, which must lie strictly
 * inside [vma->start, vma->end).  After the call the original VMA
 * covers [start, boundary) and a freshly allocated VMA covers
 * [boundary, end).  The new upper half is returned through `*out`.
 */
int vma_split_at(struct rb_root *root, struct vma *vma, vaddr_t boundary,
		 struct vma **out)
{
	struct vma *upper;
	vaddr_t original_end;
	int err;

	if (!root || !vma || !out) {
		return -EINVAL;
	}
	if (boundary <= vma->start || boundary >= vma->end) {
		return -EINVAL;
	}
	if (boundary & PAGE_MASK) {
		return -EINVAL;
	}

	upper = kzalloc(sizeof(*upper));
	if (!upper) {
		return -ENOMEM;
	}

	upper->start = boundary;
	upper->end = vma->end;
	upper->flags = vma->flags;

	/*
	 * Remove the original, shrink it, then re-insert both halves.
	 * Removal + re-insertion is the simplest way to keep the
	 * rb-tree invariants consistent when the key (start addr)
	 * range changes. We save original_end so any failure below
	 * can restore the tree to its pre-split state.
	 */
	original_end = vma->end;
	vma_remove(root, vma);
	vma->end = boundary;

	err = vma_insert(root, vma);
	if (err) {
		/*
		 * Re-insert of the shrunken original failed (only
		 * possible via -EEXIST, which would mean the tree was
		 * concurrently modified or corrupted). Restore the
		 * original range and put it back so the caller observes
		 * a tree identical to the pre-call state.
		 */
		vma->end = original_end;
		(void)vma_insert(root, vma);
		kfree(upper);
		return err;
	}

	err = vma_insert(root, upper);
	if (err) {
		/*
		 * Critical rollback path: vma was already shrunk and
		 * re-inserted; if we returned now, the range
		 * [boundary, original_end) would be silently absent
		 * from the tree even though its PTEs still exist.
		 * Undo the shrink so the tree matches the page tables.
		 */
		vma_remove(root, vma);
		vma->end = original_end;
		(void)vma_insert(root, vma);
		kfree(upper);
		return err;
	}

	*out = upper;
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Range removal — v0.0.3 §4.2                                               */
/* ------------------------------------------------------------------------- */

/*
 * Remove every VMA fully inside [start, end).  VMAs that straddle a
 * boundary are split first so only the interior portion is removed.
 * Freed VMA descriptors are kfree'd.  If `space` is non-NULL, PTEs
 * in the removed ranges are torn down via paging_unmap.
 */
int vma_remove_range(struct rb_root *root, struct addr_space *space,
		     vaddr_t start, vaddr_t end)
{
	struct vma *v;
	struct vma *next;
	struct vma *discard;
	int err;

	if (!root || start >= end) {
		return -EINVAL;
	}

	/* Split a VMA that straddles the left boundary. */
	v = vma_find(root, start);
	if (v && v->start < start) {
		err = vma_split_at(root, v, start, &discard);
		if (err) {
			return err;
		}
		/* `discard` starts at `start`; will be removed below. */
	}

	/* Split a VMA that straddles the right boundary. */
	v = vma_find(root, end - 1);
	if (v && v->end > end) {
		err = vma_split_at(root, v, end, &discard);
		if (err) {
			return err;
		}
		/* `v` now ends at `end`; will be removed below. */
	}

	/*
	 * Walk forward from `start` and remove every VMA whose range
	 * falls entirely inside [start, end).
	 */
	v = vma_find(root, start);
	if (!v) {
		/* No VMA at `start` — advance to the first one past it. */
		v = vma_first(root);
		while (v && v->end <= start) {
			v = vma_next(v);
		}
	}

	while (v && v->start < end) {
		next = vma_next(v);

		if (v->start >= start && v->end <= end) {
			if (space) {
				paging_unmap(space, v->start,
					     (v->end - v->start) / PAGE_SIZE);
			}
			vma_remove(root, v);
			kfree(v);
		}

		v = next;
	}

	return 0;
}

/* ------------------------------------------------------------------------- */
/* Top-down gap finder — v0.0.3 §2.3                                         */
/* ------------------------------------------------------------------------- */

/*
 * Find the highest gap of at least `size` page-aligned bytes below
 * MMAP_BASE.  Walk all VMAs low-to-high, tracking the gap above each
 * VMA.  After the walk, also check the gap between the last VMA and
 * the ceiling.  Return the base address of the best (highest) gap,
 * or 0 if none fits.
 */
vaddr_t vma_find_gap_top_down(const struct rb_root *root, size_t size,
			      vaddr_t ceiling)
{
	const vaddr_t floor = PAGE_SIZE; /* lowest usable user VA */
	const struct vma *v;
	vaddr_t prev_end;
	vaddr_t best = 0;

	if (size == 0 || (size & PAGE_MASK)) {
		return 0;
	}

	v = vma_first(root);
	if (!v) {
		/* Empty tree — the whole user range is free. */
		if (ceiling >= floor + size) {
			return ceiling - size;
		}
		return 0;
	}

	/*
	 * Walk every VMA below the ceiling.  For each gap between
	 * consecutive VMAs (or between `floor` and the first VMA),
	 * compute a top-down candidate: start = gap_end - size.
	 * Keep the highest candidate seen.
	 */
	prev_end = floor;

	for (; v; v = vma_next(v)) {
		if (v->start >= ceiling) {
			break;
		}

		if (v->start > prev_end && v->start - prev_end >= size) {
			vaddr_t candidate = v->start - size;

			if (candidate >= prev_end && candidate >= floor) {
				if (best == 0 || candidate > best) {
					best = candidate;
				}
			}
		}

		prev_end = v->end;
	}

	/* Gap between the last VMA (or floor) and the ceiling. */
	if (prev_end < ceiling && ceiling - prev_end >= size) {
		vaddr_t candidate = ceiling - size;

		if (candidate >= prev_end) {
			if (best == 0 || candidate > best) {
				best = candidate;
			}
		}
	}

	return best;
}
