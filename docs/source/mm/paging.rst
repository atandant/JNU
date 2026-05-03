Paging
======

The paging subsystem manages x86_64 4-level page tables (PML4 → PDPT →
PD → PT). It operates below the VMM: where the VMM tracks logical virtual
memory areas, the paging layer owns the physical page-table structures and
the TLB.

Page Size and Constants
-----------------------

.. list-table::
   :header-rows: 1
   :widths: 30 20 50

   * - Constant
     - Value
     - Description
   * - ``PAGE_SHIFT``
     - 12
     - Log2 of the base page size.
   * - ``PAGE_SIZE``
     - 4096
     - Base page size in bytes.
   * - ``PAGE_MASK``
     - ``0xFFF``
     - Mask for the offset within a page.
   * - ``PAGE_HUGE_SHIFT``
     - 21
     - Log2 of a 2 MiB large page.
   * - ``PAGE_HUGE_SIZE``
     - 2097152
     - 2 MiB large page size in bytes.

PTE Flag Bits
-------------

Each Page Table Entry is a 64-bit word. The architectural flags are:

.. list-table::
   :header-rows: 1
   :widths: 20 15 65

   * - Symbol
     - Bit
     - Meaning
   * - ``PTE_PRESENT``
     - 0
     - Entry is valid and accessible.
   * - ``PTE_WRITE``
     - 1
     - Page is writable. Cleared for CoW pages; the VMA retains ``VMA_WRITE``.
   * - ``PTE_USER``
     - 2
     - Page is accessible from ring 3. Must be set for all user mappings.
   * - ``PTE_PWT``
     - 3
     - Page Write-Through caching.
   * - ``PTE_PCD``
     - 4
     - Page Cache Disable. Used for MMIO mappings.
   * - ``PTE_ACCESSED``
     - 5
     - Set by hardware on first access.
   * - ``PTE_DIRTY``
     - 6
     - Set by hardware on first write.
   * - ``PTE_HUGE``
     - 7
     - At PD level: maps a 2 MiB page directly.
   * - ``PTE_GLOBAL``
     - 8
     - TLB entry survives CR3 writes. Used for kernel mappings.
   * - ``PTE_NX``
     - 63
     - No-Execute. Requires ``EFER.NXE``; set on all non-code pages.

Physical address bits are masked with ``PTE_ADDR_MASK``
(``0x000FFFFFFFFFF000``).

Initialization
--------------

``paging_init(hhdm_offset)`` performs the following:

1. Allocates a fresh PML4 from the PMM.
2. Copies all kernel-half PDPT pointers from the Limine-installed page
   tables into the new PML4 using ``paging_clone_kernel_half()``.
3. Loads the new PML4 into CR3.
4. Stores the PML4 pointer for retrieval via ``paging_kernel_pml4()``.

This transfer from the bootloader's ephemeral page tables to a
kernel-controlled PML4 must complete before the PMM frees Limine's
reclaimable memory.

HHDM Mapping
------------

All physical memory that Limine maps via the Higher Half Direct Map is
accessible at ``phys_to_virt(pa) = pa + hhdm_offset``. The inverse is
``virt_to_phys(v) = v - hhdm_offset``.

Regions that are not part of the HHDM (RESERVED entries in the memory
map, BIOS ROM, ACPI tables) can be dynamically mapped with
``paging_ensure_hhdm(base, len)``. This function creates 4 KiB mappings
for any physical pages in the range that are not already covered, including
pages that fall inside 2 MiB or 1 GiB huge-page regions in the Limine map.

.. note::

   ``paging_ensure_hhdm()`` must be called after both ``pmm_init()`` and
   ``paging_init()``. The APIC subsystem calls it to ensure the LAPIC and
   IOAPIC MMIO windows are reachable.

API Reference
-------------

.. code-block:: c

   int paging_map(struct addr_space *space, vaddr_t virt,
                  paddr_t phys, size_t pages, uint64_t flags);

Maps ``pages`` 4 KiB pages from ``phys`` to ``virt`` in ``space``. Missing
intermediate page-table levels are allocated from the PMM. Returns 0 or
a negative errno.

.. code-block:: c

   int paging_unmap(struct addr_space *space, vaddr_t virt, size_t pages);

Removes the PTEs for the range. Does not free backing physical pages; the
caller (typically the VMM) is responsible for managing the physical page
lifetime.

.. code-block:: c

   int paging_protect(struct addr_space *space, vaddr_t virt,
                      size_t pages, uint64_t new_flags);

Modifies the flags of existing PTEs in place. Used by the CoW resolution
path to re-add ``PTE_WRITE`` after duplicating a physical page.

.. code-block:: c

   int paging_get_flags(struct addr_space *space, vaddr_t virt,
                        uint64_t *out_flags);

Walks the page table to retrieve the current PTE flags for a single page.

TLB Management
--------------

``paging_invlpg(vaddr_t v)`` is an inline wrapper around the ``INVLPG``
instruction. It must be called after any PTE modification that changes the
visible permissions or physical target of a page. In the single-CPU build,
no remote TLB shootdown is required; the SMP extension path will issue
``VEC_RESCHED_IPI`` broadcasts.

``paging_read_cr2()`` is an inline that reads the CR2 register, which
holds the faulting virtual address at the time of a ``#PF``.

Kernel-Half Sharing
-------------------

When a new user address space is created, ``paging_clone_kernel_half()``
copies the upper-half PDPT pointers from the boot PML4 into the new PML4.
This allows all processes to share kernel mappings without per-space
updates. Any kernel mapping added after process creation must be installed
in the boot PML4 and the shared PDPT; the user PML4 picks it up on next
access because the PDPT pointer is shared.

``paging_destroy_user_half()`` frees all user-half page-table pages in a
PML4. It does not free the physical pages backing user mappings; the VMM
does that via ``pmm_put_user_page()``.
