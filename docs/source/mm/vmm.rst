Virtual Memory Manager
======================

The Virtual Memory Manager (VMM) owns the per-process concept of an
*address space*. It couples a physical PML4 (managed by the paging layer)
with a red-black tree of Virtual Memory Areas (VMAs) that record the
logical permissions of each mapped range.

Address Space
-------------

Each process holds exactly one ``struct addr_space``:

.. code-block:: c

   struct addr_space {
       uint64_t *pml4;       /* HHDM-virtual pointer to the PML4 page */
       paddr_t   pml4_phys;  /* Physical address, loaded into CR3 */
       struct rb_root vmas;  /* Red-black tree of struct vma */
   };

The kernel itself has an address space returned by ``vmm_kernel_space()``.
User processes receive a freshly allocated space from ``vmm_create_space()``.
``vmm_switch_to(space)`` loads ``space->pml4_phys`` into CR3.

VMA Flags
---------

VMA permission flags are independent of the PTE flags. The VMA records the
*intended* permissions; the PTE may have ``PTE_WRITE`` cleared for CoW
purposes while the VMA retains ``VMA_WRITE``.

.. list-table::
   :header-rows: 1
   :widths: 20 15 65

   * - Flag
     - Bit
     - Meaning
   * - ``VMA_READ``
     - 0
     - Region is readable by the owning process.
   * - ``VMA_WRITE``
     - 1
     - Region is writable. Write faults are CoW-resolved if ``PTE_WRITE``
       is clear.
   * - ``VMA_EXEC``
     - 2
     - Region is executable. Absence implies ``PTE_NX`` on all PTEs.
   * - ``VMA_USER``
     - 3
     - Region belongs to user space. ``PTE_USER`` is set on all PTEs.

Mapping and Unmapping
---------------------

.. code-block:: c

   int vmm_map(struct addr_space *space, vaddr_t virt,
               paddr_t phys, size_t pages, uint32_t flags);

Inserts a VMA covering ``[virt, virt + pages * PAGE_SIZE)`` into the RB
tree and calls ``paging_map()`` to install the PTEs. Returns 0 or a negative
errno. Fails with ``-EEXIST`` if the range overlaps an existing VMA.

.. code-block:: c

   int vmm_unmap(struct addr_space *space, vaddr_t virt, size_t pages);

Removes the VMA from the tree, calls ``paging_unmap()`` to clear the PTEs,
and releases the physical pages via ``pmm_put_user_page()`` for each page
that was marked ``VMA_USER``.

.. code-block:: c

   int vmm_protect(struct addr_space *space, vaddr_t virt,
                   size_t pages, uint32_t new_flags);

Updates the VMA flags and calls ``paging_protect()`` to modify the PTEs.
Used by the ELF loader to apply segment-specific permissions (``PT_LOAD``
segments with different ``p_flags``).

Copy-on-Write
-------------

``vmm_clone_space(src, &dst)`` implements the address-space duplication
required by ``fork()``:

1. A new PML4 is allocated and the kernel half is copied in.
2. For every VMA in ``src`` with ``VMA_USER``:

   a. A new ``struct vma`` with the same range and flags is inserted into
      ``dst``.
   b. Every PTE in the range has ``PTE_WRITE`` cleared and a reference
      is bumped via ``pmm_get_user_page()``.
   c. The same physical pages are mapped read-only into ``dst`` at the
      same virtual addresses.

After cloning, both the parent and child observe the shared pages as
read-only. The first write by either party triggers a ``#PF`` with
``PF_EC_W | PF_EC_P | PF_EC_U``, which is handled by
``vmm_handle_cow_fault()``:

1. The faulting VMA is found in the current address space.
2. The current PTE's physical address is read.
3. If ``pmm_user_refcount(pa) > 1``, a new page is allocated, the content
   is copied, the old page is released with ``pmm_put_user_page()``, and
   the PTE is updated to point to the new page with ``PTE_WRITE`` restored.
4. If the refcount is already 1, ``PTE_WRITE`` is simply restored in place
   (no copy needed; this process is the sole owner).
5. ``paging_invlpg()`` flushes the TLB entry.

Address Space Destruction
--------------------------

``vmm_destroy_space(space)`` iterates every user VMA, unmaps it (releasing
physical pages), then frees the page-table tree via
``paging_destroy_user_half()`` and frees the PML4 page itself.

Selftests
---------

``vmm_selftest()`` verifies map, protect, and CoW semantics. It also calls
``clone_space_selftest()``, which creates a parent space, writes a pattern,
clones it, mutates the child, and verifies that the parent's mapping is
unchanged.
