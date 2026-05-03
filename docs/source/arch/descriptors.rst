Descriptor Tables
=================

GDT and TSS
-----------

The Global Descriptor Table is seven entries wide, ordered to satisfy the
``SYSRET`` constraint that user CS must equal user DS plus 8:

.. list-table::
   :header-rows: 1
   :widths: 10 20 20 50

   * - Index
     - Selector
     - Symbol
     - Description
   * - 0
     - ``0x00``
     - ``GDT_NULL``
     - Mandatory null descriptor.
   * - 1
     - ``0x08``
     - ``GDT_KERNEL_CS``
     - 64-bit kernel code segment (DPL=0).
   * - 2
     - ``0x10``
     - ``GDT_KERNEL_DS``
     - Kernel data segment (DPL=0).
   * - 3
     - ``0x18``
     - ``GDT_USER_DS``
     - User data segment (DPL=3).
   * - 4
     - ``0x20``
     - ``GDT_USER_CS``
     - 64-bit user code segment (DPL=3).
   * - 5–6
     - ``0x28``
     - ``GDT_TSS``
     - 16-byte system descriptor encoding the Task State Segment.

The TSS holds seven IST stack pointers. The following IST slots are
currently assigned:

.. list-table::
   :header-rows: 1
   :widths: 10 15 75

   * - IST
     - Symbol
     - Exception
   * - 1
     - ``IST_DF``
     - Double fault (#DF, vector 8). Requires an independent stack because
       a double fault may occur when RSP is corrupted.
   * - 2
     - ``IST_NMI``
     - Non-Maskable Interrupt. Must use a known-good stack.
   * - 3
     - ``IST_MC``
     - Machine check (#MC, vector 18).
   * - 4
     - ``IST_PF``
     - Page fault (#PF, vector 14). Separated so a stack-overflow page fault
       does not immediately triple-fault.

``tss_set_rsp0(rsp0)`` updates the RSP0 field of the active TSS. It is
called once at boot to the aligned top of the boot stack, and must be
called again on every context switch once multi-threading is implemented.

IDT
---

The IDT contains 256 interrupt-gate descriptors (IF is cleared on entry
to all handlers). ISR stub generation is handled in ``isr.S``. Vectors
that the CPU does not push an error code for (all except 8, 10, 11, 12,
13, 14, 17, 21, 29, 30) receive a synthetic ``error_code = 0`` pushed by
the stub before jumping to ``isr_common``.

``isr_common`` saves the full general-purpose register file as a
``struct cpu_state`` on the kernel stack and calls ``interrupt_dispatch()``.

The ``struct cpu_state`` layout is defined in ``idt.h`` and must exactly
match the push order in ``isr.S``:

.. code-block:: c

   struct cpu_state {
       /* GPRs pushed by isr_common (high to low): */
       uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
       uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

       /* Pushed by the per-vector stub: */
       uint64_t vector;
       uint64_t error_code;

       /* Pushed by the CPU on exception entry: */
       uint64_t rip;
       uint64_t cs;
       uint64_t rflags;
       uint64_t rsp;
       uint64_t ss;
   };

Runtime handlers are registered via ``idt_set_handler(vector, fn)``. This
is used by IOAPIC-routed devices (PS/2 keyboard, COM1 RX) and by the LAPIC
timer.

APIC and Vector Assignments
---------------------------

After the PIC is remapped and masked, all external interrupts are routed
through the IOAPIC. Vector assignments (defined in ``apic.h``) are:

.. list-table::
   :header-rows: 1
   :widths: 20 15 65

   * - Symbol
     - Vector
     - Source
   * - ``VEC_TIMER``
     - 32
     - Legacy PIT IRQ 0 (jiffies; masked after LAPIC timer init).
   * - ``VEC_KBD``
     - 33
     - PS/2 keyboard IRQ 1.
   * - ``VEC_COM1``
     - 34
     - COM1 serial receive IRQ 4.
   * - ``VEC_LAPIC_TIMER``
     - 48
     - LAPIC timer, used as the primary scheduler tick from v0.0.2.
   * - ``VEC_RESCHED_IPI``
     - 254
     - Reserved for future SMP reschedule IPI.
   * - ``VEC_SPURIOUS``
     - 255
     - LAPIC spurious interrupt. Handler performs EOI only.
