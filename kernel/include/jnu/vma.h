/*
 * include/jnu/vma.h — Per-address-space VMA tree primitives.
 *
 * Operations on the rbtree of `struct vma` inside a `struct addr_space`.
 * The vma type itself is declared in <jnu/vmm.h>.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>
#include <jnu/vmm.h>

/*
 * Insert `vma` into the tree. Rejects overlap; returns -EEXIST if the
 * range already overlaps an existing VMA, 0 on success.
 */
int vma_insert(struct rb_root *root, struct vma *v);

/* Find the VMA containing `addr`, or NULL. O(log n). */
struct vma *vma_find(const struct rb_root *root, vaddr_t addr);

void vma_remove(struct rb_root *root, struct vma *v);
