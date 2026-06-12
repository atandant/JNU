CPU Initialization
==================

The CPU bring-up is performed by ``cpu_init()``, called early in
``kernel_main()`` — immediately after ``klog_init()`` and before the
banner — so TSC timestamps are live for the rest of bring-up. The per-CPU
block is a statically allocated ``struct cpu`` accessed via the
``IA32_GS_BASE`` MSR.

Per-CPU Block
-------------

.. code-block:: c

   struct cpu {
       uint32_t id;          /* xAPIC ID of this logical CPU */
       uint64_t tsc_per_us;  /* TSC ticks per microsecond, set by cpu_calibrate_tsc() */
       bool     has_smep;
       bool     has_smap;
       bool     has_nx;
       bool     has_apic;
   };

Only a single CPU is supported in v0.0.3. ``cpu_current()`` reads
``IA32_GS_BASE`` to return a pointer to the per-CPU block. The SMP
extension path will allocate one block per LAPIC ID and program each
CPU's ``IA32_GS_BASE`` at bring-up.

Feature Detection and Control Register Setup
--------------------------------------------

``cpu_init()`` runs the following sequence:

1. **CPUID leaf 0x01** — checks for APIC, MSR, TSC. Panics if any is absent.
2. **CPUID leaf 0x07 (sub-leaf 0)** — detects SMEP (EBX bit 7) and SMAP
   (EBX bit 20).
3. **EFER.NXE** — set unconditionally; panics if NX support is absent.
4. **CR0.WP** — Write-Protect bit prevents ring-0 from writing to read-only
   pages, enforcing kernel copy-on-write correctness.
5. **CR4.PGE** — enables global pages. Kernel PTE_GLOBAL entries survive
   CR3 switches.
6. **CR4.SMEP** — set if available. Prevents ring-0 from executing pages
   that are user-accessible.
7. **CR4.SMAP** — set if available. Prevents ring-0 from accessing user
   pages without an explicit ``stac``/``clac`` window.

.. warning::

   SMEP and SMAP are enforced at the hardware level. Any kernel path that
   dereferences a user pointer without going through the helpers in
   ``usercopy.h`` will trigger a ``#PF`` with error code bit ``PF_EC_RSVD``
   clear and ``PF_EC_P`` set, causing ``panic_with_state()``.

8. **FPU eager-save** — ``fpu_init_early()``.
9. **Early TSC calibration** — ``cpu_calibrate_tsc()`` via PIT channel 2
   polling (does not require ``pit_init()``).

``cpu_mark_boot()`` records the TSC at kernel entry (called from
``kernel_main()`` before ``klog_init()``). ``cpu_us_since_boot()`` returns
microseconds since that anchor.

TSC Calibration
---------------

``cpu_calibrate_tsc()`` measures the TSC tick rate against either the HPET
(if available) or PIT channel 2. It runs during ``cpu_init()`` for early
klog timestamps and may run again after ``hpet_init()`` when HPET is
present for a tighter rate.

- HPET path: arms the HPET counter, spins for a fixed interval, and divides
  the observed TSC delta by the elapsed nanoseconds.
- PIT path: uses channel 2 in one-shot mode gated by the speaker port,
  polling until expiry.

The result is stored as ``tsc_per_us`` in the per-CPU block. After
calibration, ``cpu_us_since_boot()`` provides a monotonic microsecond
timestamp derived from ``rdtsc`` relative to the boot anchor.

MSR Accessors
-------------

Two inline helpers wrap the ``rdmsr`` and ``wrmsr`` instructions:

.. code-block:: c

   uint64_t rdmsr(uint32_t msr);
   void     wrmsr(uint32_t msr, uint64_t v);

Relevant MSR addresses defined in ``cpu.h``:

.. list-table::
   :header-rows: 1
   :widths: 30 20 50

   * - Symbol
     - Address
     - Purpose
   * - ``MSR_EFER``
     - ``0xC0000080``
     - Extended Feature Enable Register (NXE bit).
   * - ``MSR_GS_BASE``
     - ``0xC0000101``
     - GS base for the current ring. Set to per-CPU block in ring 0.
   * - ``MSR_KERNEL_GS_BASE``
     - ``0xC0000102``
     - Swapped with ``MSR_GS_BASE`` on ``SWAPGS``.
   * - ``MSR_APIC_BASE``
     - ``0x0000001B``
     - APIC base address and enable flags.
