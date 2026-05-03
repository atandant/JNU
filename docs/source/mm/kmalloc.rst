kmalloc and kfree
=================

``kmalloc`` and ``kfree`` are the general-purpose dynamic memory allocation
interface for kernel code. They sit on top of the slab allocator for small
objects and fall back to the PMM for allocations larger than a page.

API
---

.. code-block:: c

   void *kmalloc(size_t size);

Allocates at least ``size`` bytes of kernel memory. The actual allocation
is rounded up to the next power-of-two size class. Returns a pointer to
zeroed memory on success, or ``NULL`` if the underlying slab cache or PMM
cannot satisfy the request.

.. code-block:: c

   void *kzalloc(size_t size);

Identical to ``kmalloc`` but guarantees that the returned memory is zeroed.
In the current implementation, ``kmalloc`` already returns zeroed memory,
so ``kzalloc`` is an alias.

.. code-block:: c

   void kfree(void *ptr);

Returns the memory at ``ptr`` to its originating allocator. The pointer
must have been returned by ``kmalloc`` or ``kzalloc``. Passing a pointer
obtained from any other source is undefined behavior.

Size-Class Ladder
-----------------

For allocations of ``size <= PAGE_SIZE`` (4096 bytes), ``kmalloc`` selects
the smallest power-of-two size class that can accommodate the request and
allocates from the corresponding ``kmem_cache``. The caches are initialized
by ``slab_init()`` and cover sizes 8, 16, 32, 64, 128, 256, 512, 1024, and
2048 bytes.

Allocations larger than ``PAGE_SIZE`` are passed directly to
``pmm_alloc_zeroed_pages()`` at the smallest order that covers the
requested size. These allocations are tracked separately so ``kfree`` can
call ``pmm_free_pages()`` with the correct order.

.. note::

   Because large allocations go directly to the PMM, they are always a
   multiple of ``PAGE_SIZE``. Requesting, for example, 5000 bytes will
   consume 8192 bytes (order 1). This waste is intentional; large kernel
   allocations are infrequent and the simplicity is preferred over a
   general-purpose large-object cache.

Usage Notes
-----------

- Callers must check the return value for ``NULL`` and handle allocation
  failure gracefully. The kernel does not have an out-of-memory killer.
- ``kfree(NULL)`` is a no-op.
- Memory returned by ``kmalloc`` must not be passed to
  ``pmm_free_pages()`` or ``kmem_cache_free()`` directly.
