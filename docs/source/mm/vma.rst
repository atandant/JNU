Virtual Memory Areas
====================

A Virtual Memory Area (VMA) is the atomic unit of logical address-space
management. Each ``struct vma`` records a contiguous virtual range
``[start, end)`` and the *intended* permissions (read, write, exec, user).
VMAs live in a per-address-space red-black tree keyed by ``start``.

The paging layer holds the *actual* PTE flags. They can diverge during
copy-on-write: a VMA may retain ``VMA_WRITE`` while ``PTE_WRITE`` is
cleared on shared pages until a write fault resolves CoW.

Source: ``kernel/mm/vma.c``, ``include/jnu/mm/vma.h``. Tree mechanics
(insert, erase, iterate) come from :doc:`/infra/rbtree`.

Data structure
--------------

.. code-block:: c

   struct vma {
       struct rb_node rb;    /* Intrusive RB-tree node (key = start). */
       vaddr_t        start;
       vaddr_t        end;   /* Exclusive upper bound. */
       uint32_t       flags; /* VMA_READ | VMA_WRITE | VMA_EXEC | VMA_USER */
   };

Ranges are half-open and must be page-aligned on both ends. Zero-length
VMAs are invalid.

Flag bits
---------

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Flag
     - Meaning
   * - ``VMA_READ``
     - Region is readable by the owner.
   * - ``VMA_WRITE``
     - Logically writable; CoW may clear ``PTE_WRITE`` temporarily.
   * - ``VMA_EXEC``
     - Executable; absence implies ``PTE_NX`` on mapped pages.
   * - ``VMA_USER``
     - User-accessible mapping; ``PTE_USER`` set. Required for ring-3
       access. Kernel-only VMAs omit this flag.

Core API
--------

.. code-block:: c

   int vma_insert(struct rb_root *root, struct vma *v);

Insert ``v`` into the tree. Returns ``-EEXIST`` if the range overlaps
any existing VMA, 0 on success. Runs in ``O(log n)`` time.

.. code-block:: c

   struct vma *vma_find(const struct rb_root *root, vaddr_t addr);

Return the VMA containing ``addr``, or ``NULL``. Used by the ``#PF``
handler and ``mmap``/``mprotect`` paths.

.. code-block:: c

   void vma_remove(struct rb_root *root, struct vma *v);

Unlink from the tree. Does **not** ``kfree`` the descriptor — caller owns
lifetime.

.. code-block:: c

   struct vma *vma_first(const struct rb_root *root);
   struct vma *vma_next(const struct vma *v);

In-order iteration over VMAs (low address to high).

Splitting and range removal (v0.0.3)
------------------------------------

``mmap``, ``munmap``, and ``mprotect`` need to carve or delete partial
ranges without leaving holes between VMA metadata and page tables.

.. code-block:: c

   int vma_split_at(struct rb_root *root, struct vma *vma,
                    vaddr_t boundary, struct vma **out);

Split ``vma`` at page-aligned ``boundary`` (strictly inside the range).
After success, the original covers ``[start, boundary)`` and a new VMA
(allocated via ``kmalloc``) covers ``[boundary, end)``. Both halves
inherit the same flags. On failure the tree is restored to its pre-call
state.

.. code-block:: c

   int vma_remove_range(struct rb_root *root, struct addr_space *space,
                        vaddr_t start, vaddr_t end);

Remove every VMA fully inside ``[start, end)``. VMAs straddling a
boundary are split first. Freed descriptors are ``kfree``'d. When
``space`` is non-NULL, ``paging_unmap()`` tears down PTEs in removed
ranges.

Gap finding for mmap
--------------------

Anonymous ``mmap`` allocates from the top of user virtual memory downward
(Linux-style high mmap region):

.. code-block:: c

   vaddr_t vma_find_gap_top_down(const struct rb_root *root,
                                 size_t size, vaddr_t ceiling);

Walk VMAs low-to-high, track gaps between consecutive regions (and
between the floor and the first VMA), and return the **highest** gap of
at least ``size`` bytes below ``ceiling``. Returns 0 if no gap fits.

``sys_mmap`` in ``kernel/mm/mmap.c`` uses this with ``MMAP_BASE`` as the
ceiling. Empty trees treat the whole user range below the ceiling as one
gap.

Consumers
---------

.. list-table::
   :header-rows: 1
   :widths: 28 72

   * - Subsystem
     - VMA usage
   * - :doc:`vmm`
     - ``vmm_map``/``unmap``/``protect`` insert and update VMAs alongside PTEs
   * - :doc:`/proc/exec`
     - ELF ``PT_LOAD`` segments create VMAs per segment permissions
   * - ``kernel/mm/mmap.c``
     - ``mmap``/``munmap``/``mprotect`` syscalls; lazy fault-in for anon maps
   * - :doc:`/arch/interrupts`
     - ``#PF`` handler calls ``vma_find()`` for CoW and demand paging

Invariants
----------

* No two VMAs in the same tree may overlap.
* ``start`` and ``end`` must be multiples of ``PAGE_SIZE`` (4096).
* A user mapping with neither ``VMA_READ`` nor ``VMA_EXEC`` is rejected.
* Kernel address space VMAs do not set ``VMA_USER`` and are not subject
  to CoW or user copy helpers.

Selftests
---------

VMA operations are exercised indirectly through ``vmm_selftest()`` and
``clone_space_selftest()``. ``mmap`` selftests in ``mmap.c`` verify gap
finding, split, and unmap behavior when ``selftest=1`` is set at boot.
