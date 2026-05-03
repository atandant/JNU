Interrupt and Exception Handling
=================================

Exception Dispatch
------------------

All 256 IDT vectors land in ``isr_common`` after their per-vector stub
executes. ``isr_common`` saves the full GPR file and calls
``interrupt_dispatch(struct cpu_state *st)``.

``interrupt_dispatch`` routes by vector number:

- Vectors 0–31 (architectural exceptions) are forwarded to
  ``exceptions_handle()``.
- Vectors 32–255 (external interrupts and IPIs) are dispatched through
  the runtime handler table populated by ``idt_set_handler()``.

Page Fault Handler (#PF, Vector 14)
------------------------------------

The page fault handler is one of the most complex paths in the kernel. On
entry, ``CR2`` holds the faulting virtual address and the error code contains
the following architectural bits:

.. list-table::
   :header-rows: 1
   :widths: 15 15 70

   * - Bit
     - Mask
     - Meaning when set
   * - ``PF_EC_P``
     - ``bit 0``
     - Fault caused by a protection violation (page present but access denied).
   * - ``PF_EC_W``
     - ``bit 1``
     - Fault was a write access.
   * - ``PF_EC_U``
     - ``bit 2``
     - Fault occurred while in user mode (CPL=3).
   * - ``PF_EC_I``
     - ``bit 4``
     - Instruction fetch caused the fault (NX violation).

The handler checks whether the fault qualifies for Copy-on-Write resolution.
A CoW fault satisfies all of the following:

1. ``PF_EC_U`` is set (user-mode fault).
2. ``PF_EC_W`` is set (write attempt).
3. ``PF_EC_P`` is set (page is present but write-protected).
4. ``vma_find()`` returns a VMA covering the faulting address with
   ``VMA_WRITE`` set in its flags.

If all conditions hold, ``vmm_handle_cow_fault()`` is called to duplicate
the physical page and remap the VMA with ``PTE_WRITE``.

Any fault that does not meet the CoW criteria calls ``panic_with_state()``
and produces a full register dump.

APIC EOI
---------

All external interrupts delivered through the LAPIC must be acknowledged
via ``apic_eoi()``. Forgetting to send EOI will prevent delivery of further
interrupts at the same priority level. Exception handlers do not require
EOI (the CPU acknowledges those internally).

LAPIC Timer
-----------

The LAPIC timer operates in periodic mode and is configured during
``lapic_timer_init()``. Each tick fires vector ``VEC_LAPIC_TIMER`` (48).
The handler calls ``sched_tick()``, which performs round-robin task
selection and invokes a context switch if the current task's quantum has
expired.

After ``lapic_timer_init()`` returns, the legacy PIT IRQ 0 is masked at
the IOAPIC so there is exactly one active tick source.
