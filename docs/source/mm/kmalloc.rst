kmalloc and kfree
=================

``kmalloc``, ``kzalloc``, and ``kfree`` are the general-purpose dynamic
memory interface for kernel code. They sit on top of the :doc:`slab`
allocator for small objects and fall back to the :doc:`pmm` buddy allocator
for multi-page requests.

Implementation: ``kernel/mm/slab.c`` (size-class routing and large-allocation
tracking). Public API: ``include/jnu/mm/kmalloc.h``.

API
---

.. code-block:: c

   void *kmalloc(size_t size);

Allocate at least ``size`` bytes. The request is rounded up to the next
power-of-two size class for slab allocations, or to the smallest buddy
order covering the size for large allocations. Returns a pointer to
zeroed memory, or ``NULL`` on failure.

.. code-block:: c

   void *kzalloc(size_t size);

Semantically "allocate and zero." In the current implementation ``kmalloc``
already zeroes returned memory, so ``kzalloc`` is a thin alias.

.. code-block:: c

   void kfree(void *ptr);

Return memory to its originating allocator. The pointer must have come from
``kmalloc``/``kzalloc``. ``kfree(NULL)`` is a no-op.

Passing a pointer from ``pmm_alloc_pages``, ``kmem_cache_alloc`` of a
different cache, or stack memory is undefined behavior.

Size-class ladder
-----------------

For ``size <= PAGE_SIZE`` (4096 bytes), ``kmalloc`` selects the smallest
power-of-two cache that fits:

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Class (bytes)
     - Typical consumers
   * - 8, 16, 32
     - Tiny kernel structs, list nodes
   * - 64, 128
     - ``struct vma``, small driver state
   * - 256, 512
     - Path buffers, medium structs
   * - 1024, 2048
     - Larger single objects, temporary I/O buffers

Caches are created in ``slab_init()`` after PMM and VMM are ready.

Large allocations
-----------------

When ``size > PAGE_SIZE``, ``kmalloc`` calls
``pmm_alloc_zeroed_pages()`` at the smallest order whose
``PMM_ORDER_SIZE(order) >= size``. The returned pointer is recorded in an
internal metadata table so ``kfree`` can call ``pmm_free_pages()`` with
the correct order.

.. note::

   A request for 5000 bytes consumes 8192 bytes (buddy order 1). Large
   kernel allocations are infrequent; simplicity is preferred over a
   dedicated large-object cache.

Alignment
---------

Slab objects are aligned to at least the cache's alignment (typically
natural alignment for the size class). Large buddy allocations are
page-aligned (4096 bytes).

There is no ``krealloc`` in v0.0.3. Callers that need resize must
allocate a new block, copy, and ``kfree`` the old one.

Failure behavior
----------------

``kmalloc`` returns ``NULL`` when the PMM or slab cannot satisfy the
request. There is no OOM killer; callers **must** check the return value
and propagate ``-ENOMEM`` or take a safe fallback path.

Several boot paths use ``panic()`` on allocation failure (e.g. first
process setup) where continuing would be unsafe.

Usage guidelines
----------------

* Prefer stack or static storage for hot paths and boot-before-slab code.
* Use ``kmem_cache_create`` directly when allocating many objects of the
  same type (see :doc:`slab`).
* Never ``kfree`` a pointer still reachable from another subsystem without
  a clear ownership transfer.
* Slab does not provide guard pages or use-after-free detection — treat
  freed pointers as toxic.

Boot ordering
-------------

``slab_init()`` runs in ``kernel_main()`` after ``pmm_init()``,
``paging_init()``, and ``vmm_init()``. Code before step 9 in
:doc:`/arch/boot` must not call ``kmalloc``.

Selftests
---------

``slab_selftest()`` (invoked when ``selftest=1``) allocates and frees
objects across multiple slab pages and verifies alloc/free count balance.
See :doc:`/infra/selftest`.
