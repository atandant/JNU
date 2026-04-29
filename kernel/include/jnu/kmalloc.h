/*
 * include/jnu/kmalloc.h — Generic kernel allocator on top of slab + buddy.
 *
 * `kmalloc` returns zeroed memory via a power-of-2 size-class ladder.
 * Sizes greater than PAGE_SIZE go straight to the PMM in `order` units.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>

void *kmalloc(size_t size);

void kfree(void *ptr);

/* Allocate zeroed memory. */
void *kzalloc(size_t size);
