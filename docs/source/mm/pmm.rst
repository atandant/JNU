Physical Memory Manager
=======================

The Physical Memory Manager (PMM) owns all physical RAM on the system. It
implements a binary buddy allocator with 11 orders, two zones, and a
per-page reference counting scheme for user-mapped pages.

Zones
-----

Physical memory is divided into two zones based on address:

.. list-table::
   :header-rows: 1
   :widths: 20 20 60

   * - Zone
     - Range
     - Purpose
   * - ``PMM_ZONE_DMA``
     - 0 — 16 MiB
     - ISA DMA-capable region. Used by ``pmm_alloc_dma()`` for hardware
       that cannot address above 16 MiB.
   * - ``PMM_ZONE_NORMAL``
     - 16 MiB — end of RAM
     - General-purpose allocations. Preferred by all non-DMA callers.

Buddy Allocator
---------------

Allocations are quantized to powers of two in page units. Order ``n``
allocates ``2^n`` contiguous 4 KiB pages:

.. list-table::
   :header-rows: 1
   :widths: 10 20 70

   * - Order
     - Size
     - Notes
   * - 0
     - 4 KiB
     - Single page; the most common allocation unit.
   * - 1
     - 8 KiB
     -
   * - ...
     - ...
     -
   * - 10
     - 4 MiB
     - Maximum order (``PMM_MAX_ORDER - 1 == 10``).

The macro ``PMM_ORDER_SIZE(o)`` computes the byte size of order ``o`` as
``1 << (12 + o)``.

During ``pmm_init()``, each USABLE span in the Limine memory map is added
to the free list at the highest naturally aligned order it supports. This
produces a set of free lists that is maximally coalesced at startup.

API
---

.. code-block:: c

   paddr_t pmm_alloc_pages(int order);

Allocates ``2^order`` contiguous pages. Returns the physical base address
on success, or ``0`` on failure. The caller must convert to a virtual
address via ``phys_to_virt()`` before accessing the memory.

.. code-block:: c

   paddr_t pmm_alloc_zeroed_pages(int order);

Equivalent to ``pmm_alloc_pages`` but zeroes the allocation before
returning. Required for new page-table pages and user mappings.

.. code-block:: c

   paddr_t pmm_alloc_user_page(void);

Allocates a single order-0 page for user mappings. Initializes its
reference count to 1.

.. code-block:: c

   paddr_t pmm_alloc_dma(int order);

Allocates from ``PMM_ZONE_DMA`` only. Panics if no DMA memory is
available at the requested order.

.. code-block:: c

   void pmm_free_pages(paddr_t pa, int order);

Returns ``2^order`` pages starting at ``pa`` to the free list, coalescing
with their buddy if the buddy is also free.

Reference Counting
------------------

Only pages allocated with ``pmm_alloc_user_page()`` carry a reference count.
Kernel page-table pages, slab backing, and large ``kmalloc`` allocations do
**not** use the refcount mechanism; their lifetime is governed by the
allocating subsystem.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Function
     - Effect
   * - ``pmm_get_user_page(pa)``
     - Increments the reference count for the page at ``pa``.
   * - ``pmm_put_user_page(pa)``
     - Decrements the reference count. Frees the page when the count reaches 0.
   * - ``pmm_user_refcount(pa)``
     - Returns the current reference count. Exposed for selftests only;
       not part of the kernel API.

Statistics
----------

``pmm_get_stats(struct pmm_stats *out)`` fills a ``pmm_stats`` structure:

.. code-block:: c

   struct pmm_stats {
       uint64_t total_pages;
       uint64_t free_pages;
       uint64_t free_by_order[PMM_MAX_ORDER];
       uint64_t zone_total[PMM_ZONE_NR];
       uint64_t zone_free[PMM_ZONE_NR];
   };

``pmm_dump()`` logs a human-readable summary to the kernel log. It is
invoked when the ``dump=mem`` command-line option is present.

Selftests
---------

``pmm_selftest()`` verifies basic alloc/free round-trips across orders and
checks that buddy coalescing works correctly. It is gated on ``selftest=1``.
