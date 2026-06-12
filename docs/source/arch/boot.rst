Boot Sequence
=============

The JNU kernel boots via the Limine bootloader protocol (base revision 3).
The handoff from firmware to ``kernel_main()`` is direct: Limine sets up a
higher-half identity map for the kernel image, provides a physical memory
map, and places a HHDM offset in the response structure. No secondary
bootloader stage exists.

Limine Request Layout
---------------------

All Limine requests are placed in a dedicated ELF section named
``.limine_requests`` via the ``__section`` attribute. The requests declared
by ``kernel/kernel/main.c`` and their purpose are:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Request
     - Purpose
   * - ``LIMINE_BASE_REVISION(3)``
     - Asserts that revision 3 is required; boot halts if not supported.
   * - ``limine_framebuffer_request``
     - Obtains address, pitch, dimensions, and BPP of the first framebuffer.
   * - ``limine_kernel_address_request``
     - Retrieves the physical and virtual base addresses of the kernel image.
   * - ``limine_kernel_file_request``
     - Provides the kernel ELF file descriptor, including its command line.
   * - ``limine_memmap_request``
     - Delivers the E820-style physical memory map used to initialize the PMM.
   * - ``limine_hhdm_request``
     - Provides the HHDM virtual offset (typically ``0xffff800000000000``).
   * - ``limine_rsdp_request``
     - Supplies a pointer to the ACPI RSDP, used by the APIC subsystem.
   * - ``limine_module_request``
     - Delivers attached modules; the kernel searches these for the initramfs.

.. warning::

   If ``LIMINE_BASE_REVISION_SUPPORTED`` evaluates to false at runtime,
   ``kernel_main`` enters a ``cli; hlt`` spin loop without printing any
   diagnostic. This is the only case where the kernel halts without calling
   ``panic()``.

Boot Phase Sequence
-------------------

The following table lists each bring-up step in the order it occurs in
``kernel_main()``. Steps are numbered as listed in the function-level
comment in ``main.c``.

.. list-table::
   :header-rows: 1
   :widths: 5 30 65

   * - Step
     - Subsystem
     - Notes
   * - 1
     - ``cpu_mark_boot()`` / ``klog_init()`` / ``serial_init()``
     - Anchor boot TSC; establish COM1 output. No heap yet.
   * - 2
     - ``cpu_init()``
     - CPUID, CR0/CR4/EFER, GS_BASE, early TSC calibration (PIT).
   * - 3
     - ``cmdline_parse()``
     - Parses the Limine kernel command line into a flat key=value store.
   * - 4
     - ``fbcon_init()``
     - Optional; silently skipped if no framebuffer is present.
   * - 5
     - ``gdt_init()`` / ``arch_syscall_init()`` / ``idt_init()``
     - Descriptor tables, SYSCALL MSRs, and ISR vectors installed.
   * - 6
     - ``pic_remap_and_mask()``
     - Remaps the legacy 8259 PIC to vectors 0x20–0x2F and masks all lines.
   * - 7
     - ``pmm_init()``
     - Buddy allocator initialized from the Limine memory map.
   * - 8
     - ``paging_init()``
     - Kernel PML4 created; CR3 switched; HHDM re-established under kernel control.
   * - 9
     - ``vmm_init()`` / ``slab_init()``
     - Virtual address space manager and slab allocator brought up.
   * - 10
     - ``apic_init()``
     - LAPIC and IOAPIC initialized from the ACPI MADT.
   * - 11
     - ``hpet_init()`` / ``acpi_pm_init()``
     - HPET optional; if present, ``cpu_calibrate_tsc()`` runs again.
   * - 12
     - ``pit_init()``
     - PIT 100 Hz via IOAPIC (scheduler tick source before LAPIC timer).
   * - 13
     - ``prng_seed()``
     - Seed PRNG from RDRAND/RDTSC/HPET (after timers calibrated).
   * - 14
     - ``rtc_init()``
     - Reads the CMOS RTC; logs the wall-clock time.
   * - 15
     - ``initramfs_init()``
     - Parses the CPIO-newc archive delivered as a Limine module.
   * - 16
     - ``sched_init()``
     - Round-robin task list initialized; a kernel task is created for the boot path.
   * - 17
     - ``lapic_timer_init()`` / ``ioapic_mask(0)``
     - LAPIC timer takes over as the scheduler tick; PIT IRQ 0 is masked.
   * - 18
     - ``pci_init()`` / ``virtio_blk_init()`` / ``ata_init()`` / ``kbd_init()``
     - PCI bus scan; VirtIO block (``vda``) and legacy ATA (``hda``)
       drivers register block devices; PS/2 keyboard.
   * - 19
     - ``vfs_init()`` / ``vfs_mount()``
     - VFS initialized; Minix v1 mounted at ``/`` from ``vda`` if present,
       otherwise ``hda``. Panics if neither device exists or mount fails.
   * - 20
     - ``start_init()``
     - Loads ``/init`` (or ``init=<path>``) from initramfs, creates user
       task, enters ring 3 via ``usermode_enter()``.

HHDM Address Resolution
------------------------

Limine v3 returns the RSDP address as a virtual (HHDM) pointer for RSDP
regions covered by the HHDM. Older revisions returned a physical address.
``resolve_rsdp_phys()`` disambiguates by testing whether the address lies
above ``0xFFFF800000000000``:

.. code-block:: c

   if (addr >= 0xFFFF800000000000ull)
       return addr - hhdm;
   return addr;

Initramfs Module Selection
--------------------------

``find_initramfs_module()`` searches the module list for a module whose
command line equals ``"initramfs"``. If no labeled module is found and
exactly one module is present, that module is used unconditionally. If
multiple unlabeled modules exist, ``initramfs_init`` is not called and
``kernel_main`` panics.

Command-Line Hooks
------------------

The following keys are recognized at boot. All are parsed by
``cmdline_get()`` / ``cmdline_bool()``:

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Key
     - Effect
   * - ``init=<path>``
     - Overrides the default init path of ``/init``.
   * - ``noinit=1``
     - Skips userspace entirely; the kernel enters its idle loop.
   * - ``selftest=1``
     - Runs ``selftest_run_all()`` before init. Panics on failure.
   * - ``panictest=1``
     - Unconditionally calls ``panic()``. For boot-path testing only.
   * - ``execprobe=1``
     - Validates ``/bin/hello`` and ``/hello`` without executing them.
   * - ``dump=blocks``
     - Hex-dumps the first 8 sectors of ``hda`` to the log.
   * - ``dump=mem``
     - Calls ``pmm_dump()`` to print the physical memory map.
