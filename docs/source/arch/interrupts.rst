Interrupt and Exception Handling
================================

All 256 IDT vectors dispatch through per-vector stubs in ``isr.S`` that
converge on ``isr_common``, build a ``struct cpu_state`` trap frame, and
call ``interrupt_dispatch(st)``.

Source: ``kernel/arch/x86_64/idt.c``, ``isr.S``, ``exceptions.c``.

Routing
-------

``interrupt_dispatch`` branches on vector number:

* **Vectors 0–31** — architectural exceptions → ``exceptions_handle()``.
* **Vectors 32–255** — hardware IRQs and driver-installed handlers via
  the table populated by ``idt_set_handler()``.

Exception policy (user vs kernel)
---------------------------------

``exceptions_handle()`` in ``kernel/arch/x86_64/exceptions.c`` applies
different policies by privilege level:

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - Fault origin
     - Behavior
   * - User mode (CS.CPL == 3)
     - Log the fault, kill the offending process, schedule another task.
       User bugs must not panic the kernel.
   * - Kernel mode
     - ``panic_with_state()`` with full register dump and backtrace.

For ``#PF``, user origin is confirmed via both saved CS and error-code
bit ``PF_EC_U`` (user/supervisor).

This extends beyond page faults: ``#GP``, ``#UD``, ``#DE``, alignment
checks, etc. in ring 3 terminate the process rather than halting the
machine.

Page fault handler (#PF, vector 14)
-----------------------------------

On entry, ``CR2`` holds the faulting virtual address. The error code
bits (Intel SDM Vol. 3A §4.7):

.. list-table::
   :header-rows: 1
   :widths: 15 15 70

   * - Bit
     - Mask
     - Meaning when set
   * - ``PF_EC_P``
     - bit 0
     - Protection violation (page present but access denied).
   * - ``PF_EC_W``
     - bit 1
     - Write access.
   * - ``PF_EC_U``
     - bit 2
     - Fault in user mode (CPL=3).
   * - ``PF_EC_RSVD``
     - bit 3
     - Reserved bit set in PTE.
   * - ``PF_EC_I``
     - bit 4
     - Instruction fetch (NX violation).

Resolution order for user faults
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

1. **Copy-on-write** — if ``PF_EC_U``, ``PF_EC_W``, and ``PF_EC_P`` are
   set, and ``vma_find()`` returns a VMA with ``VMA_WRITE``,
   ``vmm_handle_cow_fault()`` duplicates or promotes the page.

2. **Demand paging** — if ``PF_EC_P`` is clear (not present) and the
   address lies in a valid anonymous mmap VMA, allocate a zeroed page
   and map it.

3. **Otherwise** — user fault kills the process; kernel fault panics.

Hardware interrupt handlers
---------------------------

LAPIC timer (vector 48, ``VEC_LAPIC_TIMER``)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Fires periodically after ``lapic_timer_init()``. Handler calls
``sched_tick()`` for round-robin preemption, then ``apic_eoi()``.

Legacy PIT (vector 32, ``VEC_TIMER``)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Used during early boot for TSC calibration. After the LAPIC timer is
armed, PIT IRQ 0 is masked at the IOAPIC so only one tick source drives
the scheduler.

Keyboard (vector 33, ``VEC_KBD``)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

PS/2 keyboard IRQ. Scancodes are decoded to key events; ``/dev/kbd``
read returns a US QWERTY ASCII stream (Left-Alt Meta as ESC-prefix).
Num Lock defaults from a PS/2 identify probe (MF2 → on) with optional
``numlock=`` cmdline override.

Serial (COM1)
^^^^^^^^^^^^^

Not IRQ-driven in the current build; polled UART output for klog.

APIC end-of-interrupt
---------------------

External interrupts delivered through the LAPIC must be acknowledged with
``apic_eoi()``. Missing EOI blocks further interrupts at the same
priority. Exception vectors do not require EOI.

Context and preemption
----------------------

When ``sched_tick()`` decides to preempt, it calls ``context_switch()`` in
``context.S`` from the timer IRQ handler's kernel context. The preempted
task resumes later on its kernel stack with the same IRQ frame unwound.

``SCHED_QUANTUM_TICKS`` is 1 in v0.0.3 — every timer tick can rotate the
runqueue head. See :doc:`/proc/scheduler`.

Related documentation
---------------------

* IDT layout and IST stacks: :doc:`descriptors`
* CoW and VMA lookup: :doc:`/mm/vmm`, :doc:`/mm/vma`
* Panic output on kernel faults: :doc:`/infra/panic`
