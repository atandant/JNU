Virtual Memory Manager
======================

The Virtual Memory Manager (VMM) owns the per-process *address space*:
a PML4 page table (managed by :doc:`paging`) plus a red-black tree of
:doc:`vma` descriptors recording logical permissions for each mapped
range.

Source: ``kernel/mm/vmm.c``, ``include/jnu/mm/vmm.h``. CoW cloning:
``kernel/mm/clone_space.c``.

Address space layout
--------------------

User virtual addresses occupy the low canonical half:

.. code-block:: text

   0x0000000000000000
        │
        ├── ELF PT_LOAD segments (executable, data, bss)
        ├── mmap region (grows downward from MMAP_BASE)
        └── user stack (grows down from near USER_STACK_TOP)
   0x0000800000000000  USER_TOP (exclusive — no user mappings at or above)

``USER_TOP`` is enforced by ``user_range_ok()`` in the syscall path.
The kernel maps itself in the upper canonical half via Limine's HHDM and
the shared kernel PML4 entries copied into every user address space.

Address space structure
-----------------------

.. code-block:: c

   struct addr_space {
       uint64_t *pml4;       /* HHDM-virtual pointer to the PML4 page */
       paddr_t   pml4_phys;  /* Physical address, loaded into CR3 */
       struct rb_root vmas;  /* Red-black tree of struct vma */
   };

* ``vmm_kernel_space()`` — singleton used while running kernel code on
  behalf of no user process.
* ``vmm_create_space()`` — fresh PML4 for ``exec`` or first user process.
* ``vmm_switch_to(space)`` — load ``space->pml4_phys`` into CR3.

VMA flags vs PTE flags
----------------------

VMA permission flags record *intent*; PTE flags record *hardware* state.
During CoW, a VMA may keep ``VMA_WRITE`` while ``PTE_WRITE`` is cleared
on shared pages. The ``#PF`` handler consults the VMA to decide whether a
write fault is legal, then calls ``vmm_handle_cow_fault()``.

.. list-table::
   :header-rows: 1
   :widths: 20 15 65

   * - Flag
     - Bit
     - Meaning
   * - ``VMA_READ``
     - 0
     - Region readable by owner.
   * - ``VMA_WRITE``
     - 1
     - Logically writable; CoW may defer ``PTE_WRITE``.
   * - ``VMA_EXEC``
     - 2
     - Executable; else ``PTE_NX``.
   * - ``VMA_USER``
     - 3
     - User mapping; ``PTE_USER`` on all PTEs in range.

Mapping API
-----------

.. code-block:: c

   int vmm_map(struct addr_space *space, vaddr_t virt,
               paddr_t phys, size_t pages, uint32_t flags);

Insert a VMA for ``[virt, virt + pages * PAGE_SIZE)`` and install PTEs
via ``paging_map()``. Returns ``-EEXIST`` on overlap.

.. code-block:: c

   int vmm_unmap(struct addr_space *space, vaddr_t virt, size_t pages);

Remove overlapping VMA coverage, clear PTEs, and for user pages call
``pmm_put_user_page()`` per released frame.

.. code-block:: c

   int vmm_protect(struct addr_space *space, vaddr_t virt,
                   size_t pages, uint32_t new_flags);

Update VMA flags and PTE permissions. Used by the ELF loader for
per-segment ``p_flags`` and by ``mprotect``.

Copy-on-write (fork)
--------------------

``vmm_clone_space(src, &dst)`` implements address-space duplication for
``fork()``:

1. Allocate a new PML4; copy the kernel half from ``src``.
2. For each user VMA in ``src``, insert a matching VMA in ``dst``.
3. For each user PTE, clear ``PTE_WRITE`` and bump
   ``pmm_get_user_page()`` refcount.
4. Map the same physical pages into ``dst`` at the same virtual addresses.

The first write in either process triggers a ``#PF`` handled by
``vmm_handle_cow_fault()``:

1. Locate VMA; verify write permission.
2. Read current PTE physical address.
3. If ``pmm_user_refcount(pa) > 1``, allocate a new page, copy content,
   drop old refcount, remap with ``PTE_WRITE``.
4. If refcount is 1, restore ``PTE_WRITE`` in place (sole owner).
5. ``paging_invlpg()`` for the faulting address.

Lazy anonymous mmap
-------------------

``sys_mmap`` (``kernel/mm/mmap.c``) creates a VMA with no backing pages
initially. The first access faults with "not present"; the page fault path
allocates a zeroed user page and installs the PTE. This keeps ``mmap``
cheap for large reserved regions.

Destruction
-----------

``vmm_destroy_space(space)`` unmaps every user VMA (releasing physical
pages), destroys user half page tables via ``paging_destroy_user_half()``,
and frees the PML4 frame. Called when a process is reaped after ``wait``.

Selftests
---------

``vmm_selftest()`` verifies map, protect, and unmap on a scratch space.
``clone_space_selftest()`` forks CoW semantics: parent and child share
pages, child write does not mutate parent. Run with ``selftest=1`` at boot
(see :doc:`/infra/selftest`).
