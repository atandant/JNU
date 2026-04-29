/*
 * include/jnu/slab.h — Slab object cache and kmalloc backend.
 *
 * Slab caches manage same-sized objects pulled from PMM-allocated
 * 4 KiB slabs. `kmalloc`/`kfree` (in <jnu/kmalloc.h>) sit on top of a
 * fixed power-of-2 ladder of caches; allocations larger than a page
 * fall through to pmm_alloc_pages.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/spinlock.h>
#include <jnu/types.h>

struct slab_page;

struct kmem_cache {
	const char		*name;
	size_t			object_size;
	size_t			align;
	size_t			objects_per_slab;
	struct slab_page	*partial;
	struct slab_page	*full;
	uint64_t		alloc_count;
	uint64_t		free_count;
	struct spinlock		lock;
};

void slab_init(void);

struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align);

void *kmem_cache_alloc(struct kmem_cache *cache);

void kmem_cache_free(struct kmem_cache *cache, void *obj);

int slab_selftest(void);
