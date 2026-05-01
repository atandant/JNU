/*
 * include/jnu/rbtree.h — Generic red-black tree.
 *
 * Linux-style: callers embed a `struct rb_node` in their own type and
 * provide a comparator. The tree owns no memory.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>

enum rb_color {
	RB_RED = 0,
	RB_BLACK = 1,
};

struct rb_node {
	struct rb_node *parent;
	struct rb_node *left;
	struct rb_node *right;
	enum rb_color color;
};

struct rb_root {
	struct rb_node *root;
};

#define RB_ROOT ((struct rb_root){.root = NULL})

void rb_init(struct rb_root *root);

/*
 * Insert `node` into `root`. The caller is responsible for locating
 * the parent slot via repeated comparisons; rb_link_node + rb_insert_color
 * is the canonical idiom.
 */
void rb_link_node(struct rb_node *node, struct rb_node *parent,
		  struct rb_node **slot);
void rb_insert_color(struct rb_root *root, struct rb_node *node);

void rb_erase(struct rb_root *root, struct rb_node *node);

struct rb_node *rb_first(const struct rb_root *root);
struct rb_node *rb_next(const struct rb_node *node);

int rbtree_selftest(void);
