/*
 * kernel/mm/vma.c — Per-address-space VMA tree.
 *
 * The actual rb-tree mechanics live in lib/rbtree.c. This file is the
 * tiny adapter that orders VMAs by start address and rejects overlaps
 * on insert.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/rbtree.h>
#include <jnu/types.h>
#include <jnu/vma.h>

static struct vma *node_to_vma(struct rb_node *n)
{
	if (!n) {
		return NULL;
	}
	return (struct vma *)((uint8_t *)n -
			      __builtin_offsetof(struct vma, rb));
}

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
