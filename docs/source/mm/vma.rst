Virtual Memory Areas
====================

A Virtual Memory Area (VMA) is the atomic unit of address space management.
Each ``struct vma`` records a contiguous virtual range and the logical
permissions that apply to it. VMAs are stored in a per-address-space
red-black tree keyed by start address.

Data Structure
--------------

.. code-block:: c

   struct vma {
       struct rb_node rb;    /* Intrusive RB-tree node (key = start). */
       vaddr_t        start;
       vaddr_t        end;   /* Exclusive upper bound. */
       uint32_t       flags; /* VMA_READ | VMA_WRITE | VMA_EXEC | VMA_USER */
   };

The range is half-open: ``[start, end)``. A VMA must not be zero-length
and must be page-aligned on both ends.

API
---

.. code-block:: c

   int vma_insert(struct rb_root *root, struct vma *v);

Inserts ``v`` into the tree. Traverses the tree to find the correct
position and checks that the new range does not overlap any existing VMA.
Returns ``-EEXIST`` on overlap, 0 on success.

.. code-block:: c

   struct vma *vma_find(const struct rb_root *root, vaddr_t addr);

Returns the VMA that contains ``addr``, or ``NULL`` if none exists.
Runs in ``O(log n)`` time. Called by the ``#PF`` handler to determine
whether a faulting address belongs to a valid mapping.

.. code-block:: c

   void vma_remove(struct rb_root *root, struct vma *v);

Unlinks ``v`` from the tree. Does not free the ``struct vma``; the caller
is responsible for memory management.

Invariants
----------

- No two VMAs in the same tree may overlap.
- ``start`` and ``end`` must be multiples of ``PAGE_SIZE``.
- A VMA with ``VMA_USER`` set must also have ``VMA_READ`` set; a mapping
  that is neither readable nor executable has no valid use in user space.
- The kernel address space does not use ``VMA_USER``. Kernel VMAs are
  not subject to CoW.
