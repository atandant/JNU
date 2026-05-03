Kernel Panic
============

A kernel panic is an unrecoverable error condition. The JNU panic
implementation follows the output contract defined in §13 of jnuspec2.md.
The entire path operates without locks, without heap allocation, and
without the ring buffer, so it remains functional even when the memory
subsystem has been corrupted.

Output Format
-------------

The panic output is fixed in the following order:

1. **Headline** — ``PANIC: <message>`` for ``panic()``, or
   ``PANIC #XX`` (exception mnemonic) for ``panic_with_state()``.
2. **Exception decode** — only for ``panic_with_state()``: vector number,
   error code, and a human-readable decomposition of the error code.
3. **Faulting address** — only for ``#PF`` (vector 14): the value of CR2
   at the time of the fault.
4. **CPU / ring / task line** — ``CPU 0  ring <N>  pid=<P> tid=<T>
   task=<name> syscall=<nr>``.
5. **RIP** — ``RIP=0x<addr>   <symbol+offset>`` using the kernel symbol
   table.
6. **CS, SS, RFLAGS, RSP**.
7. **All 15 GPRs** in two-column aligned pairs (RAX–R15).
8. **Control registers** — CR0, CR2, CR3, CR4.
9. **Frame-pointer backtrace** — up to 32 frames, each annotated with
   symbol and offset.
10. **Last 32 ring-buffer lines** — replayed via ``klog_drain_tail()``.
11. **"System halted."**

Entry Points
------------

.. code-block:: c

   __noreturn void panic(const char *fmt, ...);

Formats the panic message on the stack into a 256-byte buffer (never
touches the heap). Issues ``cli``, emits the headline, captures the
current RBP for the backtrace, drains the ring-buffer tail, and spins
on ``cli; hlt``.

.. code-block:: c

   __noreturn void panic_with_state(struct cpu_state *st);

Used by exception handlers in ``exceptions.c``. In addition to the above,
it decodes the saved ``struct cpu_state`` from the ISR stack frame. CR2 is
read immediately at the start to capture the faulting address before any
kernel code could potentially modify it.

Exception Mnemonics
-------------------

``panic_with_state`` prints a short mnemonic for the following vectors:

.. list-table::
   :header-rows: 1
   :widths: 10 15 75

   * - Vector
     - Mnemonic
     - Cause
   * - 0
     - ``#DE``
     - Divide-by-zero.
   * - 1
     - ``#DB``
     - Debug exception.
   * - 2
     - ``#NMI``
     - Non-Maskable Interrupt.
   * - 3
     - ``#BP``
     - Breakpoint (``INT3``).
   * - 6
     - ``#UD``
     - Invalid opcode.
   * - 8
     - ``#DF``
     - Double fault.
   * - 13
     - ``#GP``
     - General protection fault.
   * - 14
     - ``#PF``
     - Page fault.
   * - 18
     - ``#MC``
     - Machine check.
   * - other
     - ``exception``
     - Generic label.

Page Fault Error Code Decode
-----------------------------

For ``#PF``, the error code is decoded into a human-readable triplet:

.. code-block:: text

   (write|read, protection|not-present, user|supervisor[, fetch])

The ``fetch`` qualifier is appended when bit 4 (instruction fetch) is set.

Backtrace Algorithm
-------------------

The frame-pointer backtrace walks the ``rbp`` chain:

1. The initial RBP is either the current ``%rbp`` (for ``panic()``) or
   the saved ``st->rbp`` (for ``panic_with_state()``).
2. Each iteration reads ``frame[0]`` (saved RBP) and ``frame[1]`` (return
   address) from the current frame.
3. A frame is considered valid only if the return address lies above
   ``0xFFFF800000000000`` (kernel half). User-mode addresses or zero
   terminate the walk.
4. A backward or equal RBP is detected and terminates the walk to prevent
   infinite loops on corrupt stacks.
5. Maximum depth is 32 frames.

Each frame is printed as ``#N  0x<addr>   <symbol+offset>`` using
``symbols_lookup()``. If the address is not found in the symbol table,
``(?)`` is printed instead.

Print Path Constraints
----------------------

The panic print helpers bypass all normal klog machinery:

- ``emit_raw()`` calls ``klog_panic_write()`` directly.
- ``emit_fmt()`` uses a 512-byte stack buffer and ``vsnprintf()``.
- No call to ``kmalloc()``, ``kfree()``, or any sleeping primitive.
- No spinlock acquisition.

.. warning::

   The format string passed to ``panic()`` must be a string literal or
   otherwise safe from user influence. Use ``panic("%s", msg)`` for
   dynamically constructed messages.

Symbol Table
------------

See :doc:`/infra/symbols` for the symbol table mechanism used to resolve
RIP values to function names in the backtrace.
