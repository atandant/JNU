/*
 * kernel/mm/slab.c — Slab object cache and kmalloc backend.
 *
 * Each slab is one 4 KiB physical page. We carve the page into N equal
 * objects of `cache->object_size` bytes; an in-page bitmap tracks which
 * are allocated. A `struct slab_page` header sits at the bottom of the
 * page (HHDM-virt). Caches keep a list of partially-allocated slabs and
 * a list of fully-allocated slabs. Empty slabs are returned to the PMM.
 *
 * `kmalloc` walks a fixed power-of-2 ladder of caches (16 B → 2048 B);
 * sizes above PAGE_SIZE go straight to the buddy allocator in `order`
 * units, with a small heap-of-records that maps the returned virt-addr
 * back to the order on free.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/compiler.h>
#include <jnu/errno.h>
#include <jnu/klog.h>
#include <jnu/kmalloc.h>
#include <jnu/paging.h>
#include <jnu/panic.h>
#include <jnu/pmm.h>
#include <jnu/slab.h>
#include <jnu/string.h>
#include <jnu/types.h>

/* ------------------------------------------------------------------------- */
/* Slab page header                                                           */
/* ------------------------------------------------------------------------- */

#define SLAB_BITMAP_WORDS 8 /* up to 512 objects/slab */

/*
 * v0.0.3.1 audit item 7: kfree must distinguish a slab page from a
 * large_alloc page without ambiguity.  Both layouts now begin with a
 * one-byte `kind` tag at page offset 0.  PAGE_KIND_SLAB and
 * PAGE_KIND_LARGE are intentionally non-zero so a freshly-allocated
 * (or zero-on-free) page that has not yet been initialised by either
 * code path is rejected by kfree() instead of silently misclassified.
 */
#define PAGE_KIND_SLAB 0xA1u
#define PAGE_KIND_LARGE 0xA2u

struct slab_page {
	uint8_t kind; /* PAGE_KIND_SLAB */
	uint8_t _pad[7];
	struct slab_page *next;
	struct slab_page *prev;
	struct kmem_cache *cache;
	uint32_t used;
	uint32_t capacity;
	uint64_t bitmap[SLAB_BITMAP_WORDS];
};

static void list_remove(struct slab_page **head, struct slab_page *p)
{
	if (p->prev) {
		p->prev->next = p->next;
	} else {
		*head = p->next;
	}
	if (p->next) {
		p->next->prev = p->prev;
	}
	p->prev = NULL;
	p->next = NULL;
}

static void list_push(struct slab_page **head, struct slab_page *p)
{
	p->prev = NULL;
	p->next = *head;
	if (*head) {
		(*head)->prev = p;
	}
	*head = p;
}

/* ------------------------------------------------------------------------- */
/* Cache machinery                                                            */
/* ------------------------------------------------------------------------- */

#define MAX_CACHES 32
static struct kmem_cache cache_pool[MAX_CACHES];
static size_t cache_count;

static void *first_object(struct slab_page *sp)
{
	uintptr_t base = (uintptr_t)sp;
	uintptr_t obj0 = base + sizeof(*sp);
	uintptr_t align = sp->cache->align ? sp->cache->align : 8;
	obj0 = (obj0 + align - 1) & ~(align - 1);
	return (void *)obj0;
}

static struct slab_page *new_slab(struct kmem_cache *c)
{
	paddr_t pa = pmm_alloc_pages(0);
	if (!pa) {
		return NULL;
	}

	struct slab_page *sp = phys_to_virt(pa);
	memset(sp, 0, sizeof(*sp));
	sp->kind = PAGE_KIND_SLAB;
	sp->cache = c;

	uintptr_t obj0 = (uintptr_t)first_object(sp) - (uintptr_t)sp;
	size_t avail = PAGE_SIZE - obj0;
	size_t cap = avail / c->object_size;

	if (cap > SLAB_BITMAP_WORDS * 64) {
		cap = SLAB_BITMAP_WORDS * 64;
	}
	sp->capacity = (uint32_t)cap;

	c->objects_per_slab = cap; /* recorded for stats */
	return sp;
}

static int alloc_in_slab(struct slab_page *sp)
{
	for (int w = 0; w < SLAB_BITMAP_WORDS; w++) {
		uint64_t b = sp->bitmap[w];
		if (b == ~0ull) {
			continue;
		}
		int bit = __builtin_ctzll(~b);
		int idx = w * 64 + bit;
		if ((uint32_t)idx >= sp->capacity) {
			break;
		}
		sp->bitmap[w] |= (1ull << bit);
		sp->used++;
		return idx;
	}
	return -1;
}

static void free_in_slab(struct slab_page *sp, int idx)
{
	int w = idx / 64;
	int bit = idx % 64;
	if (!(sp->bitmap[w] & (1ull << bit))) {
		panic("slab: double-free idx=%d in cache '%s'", idx,
		      sp->cache->name);
	}
	sp->bitmap[w] &= ~(1ull << bit);
	sp->used--;
}

struct kmem_cache *kmem_cache_create(const char *name, size_t size,
				     size_t align)
{
	if (cache_count >= MAX_CACHES) {
		return NULL;
	}
	if (align == 0) {
		align = 8;
	}

	/* Round up object size to alignment so adjacent objects align. */
	size = (size + align - 1) & ~(align - 1);
	if (size == 0) {
		return NULL;
	}

	struct kmem_cache *c = &cache_pool[cache_count++];
	c->name = name;
	c->object_size = size;
	c->align = align;
	c->partial = NULL;
	c->full = NULL;
	c->alloc_count = 0;
	c->free_count = 0;
	spin_lock_init(&c->lock);
	return c;
}

void *kmem_cache_alloc(struct kmem_cache *c)
{
	uint64_t flags = spin_lock_irqsave(&c->lock);

	struct slab_page *sp = c->partial;
	if (!sp) {
		sp = new_slab(c);
		if (!sp) {
			spin_unlock_irqrestore(&c->lock, flags);
			return NULL;
		}
		list_push(&c->partial, sp);
	}

	int idx = alloc_in_slab(sp);
	if (idx < 0) {
		spin_unlock_irqrestore(&c->lock, flags);
		return NULL;
	}

	if (sp->used == sp->capacity) {
		list_remove(&c->partial, sp);
		list_push(&c->full, sp);
	}

	void *obj = (uint8_t *)first_object(sp) + (size_t)idx * c->object_size;
	c->alloc_count++;

	spin_unlock_irqrestore(&c->lock, flags);
	return obj;
}

void kmem_cache_free(struct kmem_cache *c, void *obj)
{
	if (!obj) {
		return;
	}
	uint64_t flags = spin_lock_irqsave(&c->lock);

	uintptr_t page = (uintptr_t)obj & ~(uintptr_t)PAGE_MASK;
	struct slab_page *sp = (struct slab_page *)page;

	if (sp->cache != c) {
		panic("slab: free of cache '%s' obj into cache '%s'",
		      sp->cache ? sp->cache->name : "(none)", c->name);
	}

	uintptr_t off = (uintptr_t)obj - (uintptr_t)first_object(sp);
	int idx = (int)(off / c->object_size);
	bool was_full = (sp->used == sp->capacity);

	free_in_slab(sp, idx);
	c->free_count++;

	if (was_full) {
		list_remove(&c->full, sp);
		list_push(&c->partial, sp);
	}
	if (sp->used == 0) {
		list_remove(&c->partial, sp);
		paddr_t pa = virt_to_phys(sp);
		pmm_free_pages(pa, 0);
	}

	spin_unlock_irqrestore(&c->lock, flags);
}

/* ------------------------------------------------------------------------- */
/* kmalloc on top                                                             */
/* ------------------------------------------------------------------------- */

#define KMALLOC_MIN_SHIFT 4  /* 16 B */
#define KMALLOC_MAX_SHIFT 11 /* 2048 B */
#define KMALLOC_NCLASSES (KMALLOC_MAX_SHIFT - KMALLOC_MIN_SHIFT + 1)

static struct kmem_cache *kmalloc_caches[KMALLOC_NCLASSES];

void slab_init(void)
{
	cache_count = 0;
	for (int i = 0; i < KMALLOC_NCLASSES; i++) {
		size_t sz = 1ull << (i + KMALLOC_MIN_SHIFT);
		static char names[KMALLOC_NCLASSES][32];
		snprintf(names[i], sizeof(names[i]), "kmalloc-%lu",
			 (unsigned long)sz);
		/*
		 * Natural alignment: align to the size class, capped at
		 * 64 (cacheline).  This mirrors Linux SLUB's
		 * SLAB_HWCACHE_ALIGN policy and is required for structs
		 * with _Alignas(64) members (e.g. struct task's
		 * fpu_state — FXRSTOR #GPs on misaligned operands).
		 */
		size_t align = sz < 64 ? sz : 64;
		kmalloc_caches[i] = kmem_cache_create(names[i], sz, align);
		if (!kmalloc_caches[i]) {
			panic("slab: kmalloc cache init failed");
		}
	}
	pr_info("slab: %d kmalloc classes (16 B - 2 KiB) initialized\n",
		KMALLOC_NCLASSES);
}

/*
 * Large allocations: physical pages tagged with a one-byte `kind`
 * (PAGE_KIND_LARGE) at offset 0 plus a magic + order tail.  We
 * sacrifice the first sizeof(struct large_hdr) bytes of the
 * allocation; the returned pointer is offset past the header.
 *
 * v0.0.3.1 audit item 7: dispatch in kfree() is by `kind` byte at
 * page offset 0, identical to slab_page.  The magic and order fields
 * remain as a corruption sanity check inside large_free().
 */
/*
 * Padded to 64 bytes so the returned pointer from large_alloc() is
 * 64-byte aligned (pages are 4096-aligned).  This is required for
 * structs containing _Alignas(64) fields such as struct task's
 * fpu_state buffer — FXSAVE/FXRSTOR #GP on misaligned operands.
 */
struct large_hdr {
	uint8_t kind; /* PAGE_KIND_LARGE */
	uint8_t _pad[7];
	uint64_t magic;
	uint32_t order;
	uint32_t _reserved[9]; /* pad to 64 bytes total */
};

#define LARGE_MAGIC 0x534C4142504B5350ull /* "SLABPKSP" */

static void *large_alloc(size_t size)
{
	int order = 0;
	size_t req;

	if (__builtin_add_overflow(size, sizeof(struct large_hdr), &req)) {
		return NULL;
	}

	while ((size_t)(PAGE_SIZE << order) < req) {
		order++;
		if (order >= PMM_MAX_ORDER) {
			return NULL;
		}
	}
	paddr_t pa = pmm_alloc_pages(order);
	if (!pa) {
		return NULL;
	}
	struct large_hdr *h = phys_to_virt(pa);
	h->kind = PAGE_KIND_LARGE;
	h->magic = LARGE_MAGIC;
	h->order = (uint32_t)order;
	return (uint8_t *)h + sizeof(*h);
}

static void large_free(void *p)
{
	struct large_hdr *h =
	    (struct large_hdr *)((uint8_t *)p - sizeof(struct large_hdr));
	if (h->kind != PAGE_KIND_LARGE || h->magic != LARGE_MAGIC) {
		panic("kfree: corrupt large header at %p", p);
	}
	int order = (int)h->order;
	paddr_t pa = virt_to_phys(h);
	h->kind = 0;
	h->magic = 0;
	pmm_free_pages(pa, order);
}

void *kmalloc(size_t size)
{
	if (size == 0) {
		return NULL;
	}

	if (size > (1ull << KMALLOC_MAX_SHIFT)) {
		return large_alloc(size);
	}

	int shift = KMALLOC_MIN_SHIFT;
	while ((1ull << shift) < size) {
		shift++;
	}
	int idx = shift - KMALLOC_MIN_SHIFT;
	return kmem_cache_alloc(kmalloc_caches[idx]);
}

void kfree(void *p)
{
	if (!p) {
		return;
	}

	uintptr_t page = (uintptr_t)p & ~(uintptr_t)PAGE_MASK;
	/*
	 * v0.0.3.1 audit item 7: dispatch on the one-byte `kind` tag
	 * at page offset 0.  Both slab_page and large_hdr place the
	 * tag there with disjoint values, so this is unambiguous and
	 * does not depend on relative struct sizes.  An invalid tag
	 * indicates either a corrupt heap or a free of a non-kmalloc
	 * pointer; either is a kernel bug.
	 */
	uint8_t kind = *(volatile uint8_t *)page;

	if (kind == PAGE_KIND_LARGE) {
		struct large_hdr *h = (struct large_hdr *)page;

		if ((uintptr_t)p != page + sizeof(*h)) {
			panic("kfree: %p mid-large-allocation", p);
		}
		large_free(p);
		return;
	}
	if (kind == PAGE_KIND_SLAB) {
		struct slab_page *sp = (struct slab_page *)page;

		if (!sp->cache) {
			panic("kfree: %p slab header missing cache", p);
		}
		kmem_cache_free(sp->cache, p);
		return;
	}
	panic("kfree: %p has unknown page kind 0x%02x", p, kind);
}

void *kzalloc(size_t size)
{
	void *p = kmalloc(size);
	if (p) {
		memset(p, 0, size);
	}
	return p;
}

/* ------------------------------------------------------------------------- */
/* Selftest                                                                   */
/* ------------------------------------------------------------------------- */

#define SLAB_TEST_N 1024

int slab_selftest(void)
{
	void *ptrs[SLAB_TEST_N];
	struct pmm_stats before, after;

	pmm_get_stats(&before);

	/* Mixed-class kmalloc/kfree. */
	for (size_t round = 0; round < 5; round++) {
		for (size_t i = 0; i < SLAB_TEST_N; i++) {
			size_t sz = 16 + ((i * 37) % 1024);
			ptrs[i] = kmalloc(sz);
			if (!ptrs[i]) {
				pr_err("slab: kmalloc(%lu) failed\n",
				       (unsigned long)sz);
				return -ENOMEM;
			}
			((volatile uint8_t *)ptrs[i])[0] = (uint8_t)i;
		}
		for (size_t i = 0; i < SLAB_TEST_N; i++) {
			kfree(ptrs[i]);
		}
	}

	/* Large path. */
	void *big = kmalloc(8192);
	if (!big) {
		pr_err("slab: large kmalloc failed\n");
		return -ENOMEM;
	}
	memset(big, 0xA5, 8192);
	kfree(big);

	pmm_get_stats(&after);
	if (after.free_pages != before.free_pages) {
		pr_err("slab: leak: %lu -> %lu pages\n",
		       (unsigned long)before.free_pages,
		       (unsigned long)after.free_pages);
		return -EINVAL;
	}

	return 0;
}
