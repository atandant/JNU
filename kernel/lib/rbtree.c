/*
 * kernel/lib/rbtree.c — Generic intrusive red-black tree.
 *
 * Standard CLRS red-black tree, intrusive: callers embed `struct rb_node`
 * inside their own struct and provide their own comparator at insertion
 * time. The tree owns no memory and never allocates.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/rbtree.h>
#include <jnu/types.h>

void rb_init(struct rb_root *root)
{
	root->root = NULL;
}

void rb_link_node(struct rb_node *node, struct rb_node *parent,
		  struct rb_node **slot)
{
	node->parent = parent;
	node->left = NULL;
	node->right = NULL;
	node->color = RB_RED;
	*slot = node;
}

static struct rb_node *rb_grandparent(struct rb_node *n)
{
	return (n && n->parent) ? n->parent->parent : NULL;
}

static void rotate_left(struct rb_root *root, struct rb_node *x)
{
	struct rb_node *y = x->right;
	x->right = y->left;
	if (y->left) {
		y->left->parent = x;
	}
	y->parent = x->parent;
	if (!x->parent) {
		root->root = y;
	} else if (x == x->parent->left) {
		x->parent->left = y;
	} else {
		x->parent->right = y;
	}
	y->left = x;
	x->parent = y;
}

static void rotate_right(struct rb_root *root, struct rb_node *x)
{
	struct rb_node *y = x->left;
	x->left = y->right;
	if (y->right) {
		y->right->parent = x;
	}
	y->parent = x->parent;
	if (!x->parent) {
		root->root = y;
	} else if (x == x->parent->right) {
		x->parent->right = y;
	} else {
		x->parent->left = y;
	}
	y->right = x;
	x->parent = y;
}

void rb_insert_color(struct rb_root *root, struct rb_node *node)
{
	struct rb_node *z = node;

	while (z->parent && z->parent->color == RB_RED) {
		struct rb_node *gp = rb_grandparent(z);
		if (!gp) {
			break;
		}
		if (z->parent == gp->left) {
			struct rb_node *u = gp->right;
			if (u && u->color == RB_RED) {
				z->parent->color = RB_BLACK;
				u->color = RB_BLACK;
				gp->color = RB_RED;
				z = gp;
			} else {
				if (z == z->parent->right) {
					z = z->parent;
					rotate_left(root, z);
				}
				z->parent->color = RB_BLACK;
				gp = rb_grandparent(z);
				if (gp) {
					gp->color = RB_RED;
					rotate_right(root, gp);
				}
			}
		} else {
			struct rb_node *u = gp->left;
			if (u && u->color == RB_RED) {
				z->parent->color = RB_BLACK;
				u->color = RB_BLACK;
				gp->color = RB_RED;
				z = gp;
			} else {
				if (z == z->parent->left) {
					z = z->parent;
					rotate_right(root, z);
				}
				z->parent->color = RB_BLACK;
				gp = rb_grandparent(z);
				if (gp) {
					gp->color = RB_RED;
					rotate_left(root, gp);
				}
			}
		}
	}

	if (root->root) {
		root->root->color = RB_BLACK;
	}
}

static struct rb_node *rb_min(struct rb_node *n)
{
	while (n && n->left) {
		n = n->left;
	}
	return n;
}

struct rb_node *rb_first(const struct rb_root *root)
{
	return rb_min(root->root);
}

struct rb_node *rb_next(const struct rb_node *node)
{
	struct rb_node *n = (struct rb_node *)node;

	if (n->right) {
		return rb_min(n->right);
	}
	while (n->parent && n == n->parent->right) {
		n = n->parent;
	}
	return n->parent;
}

static void transplant(struct rb_root *root, struct rb_node *u,
		       struct rb_node *v)
{
	if (!u->parent) {
		root->root = v;
	} else if (u == u->parent->left) {
		u->parent->left = v;
	} else {
		u->parent->right = v;
	}
	if (v) {
		v->parent = u->parent;
	}
}

static void erase_fixup(struct rb_root *root, struct rb_node *x,
			struct rb_node *xp)
{
	while ((x != root->root) && (!x || x->color == RB_BLACK)) {
		struct rb_node *p = x ? x->parent : xp;
		if (!p) {
			break;
		}
		if (x == p->left) {
			struct rb_node *w = p->right;
			if (w && w->color == RB_RED) {
				w->color = RB_BLACK;
				p->color = RB_RED;
				rotate_left(root, p);
				w = p->right;
			}
			if ((!w) ||
			    ((!w->left || w->left->color == RB_BLACK) &&
			     (!w->right || w->right->color == RB_BLACK))) {
				if (w) {
					w->color = RB_RED;
				}
				x = p;
				xp = p->parent;
			} else {
				if (!w->right || w->right->color == RB_BLACK) {
					if (w->left) {
						w->left->color = RB_BLACK;
					}
					w->color = RB_RED;
					rotate_right(root, w);
					w = p->right;
				}
				if (w) {
					w->color = p->color;
				}
				p->color = RB_BLACK;
				if (w && w->right) {
					w->right->color = RB_BLACK;
				}
				rotate_left(root, p);
				x = root->root;
			}
		} else {
			struct rb_node *w = p->left;
			if (w && w->color == RB_RED) {
				w->color = RB_BLACK;
				p->color = RB_RED;
				rotate_right(root, p);
				w = p->left;
			}
			if ((!w) ||
			    ((!w->left || w->left->color == RB_BLACK) &&
			     (!w->right || w->right->color == RB_BLACK))) {
				if (w) {
					w->color = RB_RED;
				}
				x = p;
				xp = p->parent;
			} else {
				if (!w->left || w->left->color == RB_BLACK) {
					if (w->right) {
						w->right->color = RB_BLACK;
					}
					w->color = RB_RED;
					rotate_left(root, w);
					w = p->left;
				}
				if (w) {
					w->color = p->color;
				}
				p->color = RB_BLACK;
				if (w && w->left) {
					w->left->color = RB_BLACK;
				}
				rotate_right(root, p);
				x = root->root;
			}
		}
	}
	if (x) {
		x->color = RB_BLACK;
	}
}

void rb_erase(struct rb_root *root, struct rb_node *z)
{
	struct rb_node *y = z;
	struct rb_node *x;
	struct rb_node *xp;
	enum rb_color y_orig = y->color;

	if (!z->left) {
		x = z->right;
		xp = z->parent;
		transplant(root, z, z->right);
	} else if (!z->right) {
		x = z->left;
		xp = z->parent;
		transplant(root, z, z->left);
	} else {
		y = rb_min(z->right);
		y_orig = y->color;
		x = y->right;
		if (y->parent == z) {
			xp = y;
		} else {
			transplant(root, y, y->right);
			y->right = z->right;
			y->right->parent = y;
			xp = y->parent;
		}
		transplant(root, z, y);
		y->left = z->left;
		y->left->parent = y;
		y->color = z->color;
	}

	if (y_orig == RB_BLACK) {
		erase_fixup(root, x, xp);
	}
}

/* ------------------------------------------------------------------------- */
/* Selftest                                                                   */
/* ------------------------------------------------------------------------- */

#include <jnu/errno.h>
#include <jnu/klog.h>

struct rb_test_node {
	struct rb_node	rb;
	uint64_t	key;
};

static void rb_test_insert(struct rb_root *r, struct rb_test_node *n)
{
	struct rb_node **slot = &r->root;
	struct rb_node *parent = NULL;

	while (*slot) {
		struct rb_test_node *cur =
			(struct rb_test_node *)(*slot);
		parent = *slot;
		slot = (n->key < cur->key) ? &(*slot)->left : &(*slot)->right;
	}
	rb_link_node(&n->rb, parent, slot);
	rb_insert_color(r, &n->rb);
}

static int validate_walk(const struct rb_root *r, size_t expected)
{
	uint64_t prev = 0;
	bool first = true;
	size_t count = 0;

	for (struct rb_node *n = rb_first(r); n; n = rb_next(n)) {
		struct rb_test_node *t = (struct rb_test_node *)n;
		if (!first && t->key < prev) {
			return -EINVAL;
		}
		prev = t->key;
		first = false;
		count++;
	}
	if (count != expected) {
		return -EINVAL;
	}
	return 0;
}

#define RBTEST_N	256

static struct rb_test_node rbtest_nodes[RBTEST_N];

int rbtree_selftest(void)
{
	struct rb_root r = RB_ROOT;
	int err;

	/* Pseudo-random keys via a fixed LCG so the test is deterministic. */
	uint32_t s = 0xC0FFEEu;
	for (size_t i = 0; i < RBTEST_N; i++) {
		s = s * 1103515245u + 12345u;
		rbtest_nodes[i].key = (s >> 1) | ((uint64_t)i << 32);
		rb_test_insert(&r, &rbtest_nodes[i]);
	}

	err = validate_walk(&r, RBTEST_N);
	if (err) {
		pr_err("rbtree: post-insert walk invalid\n");
		return err;
	}

	/* Erase every other node, walk again. */
	for (size_t i = 0; i < RBTEST_N; i += 2) {
		rb_erase(&r, &rbtest_nodes[i].rb);
	}

	err = validate_walk(&r, RBTEST_N / 2);
	if (err) {
		pr_err("rbtree: post-erase walk invalid\n");
		return err;
	}

	return 0;
}
