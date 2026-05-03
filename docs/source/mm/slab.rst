Slab Allocator
==============

The slab allocator provides the kernel's general-purpose dynamic memory
allocation. It is layered immediately above the PMM and below
``kmalloc``/``kfree``.

Architecture
------------

A *slab cache* (``struct kmem_cache``) manages objects of a single fixed
size. The cache maintains two lists of *slab pages*:

- **partial**: slab pages that have at least one free object slot.
- **full**: slab pages where every slot is allocated.

Each slab page is a PMM order-0 allocation (4 KiB). The slab page header
records a free-list bitmap or linked list of free object slots within the
page.

``kmalloc``/``kfree`` sit on top of a fixed power-of-2 ladder of caches.
Allocations larger than a page fall through to ``pmm_alloc_pages()``
directly.

Data Structures
---------------

.. code-block:: c

   struct kmem_cache {
       const char       *name;
       size_t            object_size;
       size_t            align;
       size_t            objects_per_slab;
       struct slab_page *partial;
       struct slab_page *full;
       uint64_t          alloc_count;
       uint64_t          free_count;
       struct spinlock   lock;
   };

``struct slab_page`` is an internal type not exposed in ``slab.h``. The
``lock`` field protects both list pointers and the object counts.

API
---

.. code-block:: c

   void slab_init(void);

Initializes the fixed power-of-2 cache ladder used by ``kmalloc``. Must
be called after ``pmm_init()`` and ``vmm_init()``.

.. code-block:: c

   struct kmem_cache *kmem_cache_create(const char *name,
                                        size_t size, size_t align);

Allocates and returns a new cache for objects of ``size`` bytes, aligned
to ``align`` bytes. The cache itself is allocated from the internal
bootstrap cache. Returns ``NULL`` on allocation failure.

.. code-block:: c

   void *kmem_cache_alloc(struct kmem_cache *cache);

Returns a pointer to one object from the cache. If the partial list is
empty, a new slab page is allocated from the PMM. Returns ``NULL`` if the
PMM cannot satisfy the request.

.. code-block:: c

   void kmem_cache_free(struct kmem_cache *cache, void *obj);

Returns ``obj`` to its originating slab page. If the slab page becomes
fully free, it is returned to the PMM. The caller must pass the same
``cache`` that was used for ``kmem_cache_alloc``; mixing caches is a kernel
bug and will silently corrupt the free list.

.. warning::

   The slab allocator does not perform guard pages, red zones, or use-after-free
   detection in the current build. Kernel code must not access freed objects.

Selftests
---------

``slab_selftest()`` creates a temporary cache, allocates and frees objects
across multiple slab pages, and verifies that the alloc and free counts
balance correctly.
