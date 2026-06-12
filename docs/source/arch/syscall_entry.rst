Syscall Entry and Frame Layout
==============================

The JNU syscall path uses the x86_64 ``SYSCALL``/``SYSRET`` instruction
pair. ``SYSCALL`` saves user ``RIP`` in ``RCX``, user ``RFLAGS`` in
``R11``, and jumps to ``IA32_LSTAR`` (``syscall_entry`` in
``syscall_entry.S``).

MSR setup
---------

``arch_syscall_init()`` in ``kernel/arch/x86_64/arch_syscall.c`` runs
during boot (step 5 in :doc:`boot`), after GDT install:

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - MSR
     - Value / purpose
   * - ``IA32_STAR`` (``0xC0000081``)
     - Encodes kernel CS/SS selectors for ``SYSCALL``/``SYSRET`` target.
       User CS/SS are derived by adding 16/8 to STAR's user field per
       Intel rules — must match :doc:`descriptors` GDT layout.
   * - ``IA32_LSTAR`` (``0xC0000082``)
     - Entry point: ``syscall_entry``.
   * - ``IA32_FMASK`` (``0xC0000084``)
     - RFLAGS mask: clears TF, IF, DF, IOPL, NT, AC on syscall entry.
   * - ``IA32_EFER.SCE``
     - Enables the syscall mechanism.

``IA32_KERNEL_GS_BASE`` points at a per-CPU ``struct syscall_scratch``:

.. code-block:: c

   struct syscall_scratch {
       uint64_t user_rsp;    /* offset 0 — saved user stack pointer */
       uint64_t kernel_rsp;  /* offset 8 — current task kernel stack top */
       int64_t  current_nr;  /* syscall number for debugging */
   };

``arch_syscall_set_kernel_stack()`` updates ``kernel_rsp`` on every
context switch so the entry stub always switches to the correct task
stack.

Register convention
-------------------

On entry to ``syscall_entry.S``:

.. list-table::
   :header-rows: 1
   :widths: 15 85

   * - Register
     - Role
   * - ``RAX``
     - Syscall number (Linux x86_64 values — see :doc:`/syscall/table`).
   * - ``RDI``
     - Argument 0.
   * - ``RSI``
     - Argument 1.
   * - ``RDX``
     - Argument 2.
   * - ``R10``
     - Argument 3 (replaces ``RCX``, clobbered by ``SYSCALL``).
   * - ``R8``
     - Argument 4.
   * - ``R9``
     - Argument 5.
   * - ``RCX``
     - User return RIP (saved by hardware).
   * - ``R11``
     - User RFLAGS (saved by hardware).

Entry sequence (summary)
------------------------

1. ``SWAPGS`` — activate kernel GS base (scratch struct).
2. Save user ``RSP`` to scratch slot 0.
3. Load kernel ``RSP`` from scratch slot 8.
4. Push ``struct syscall_args`` and callee-saved user registers onto the
   kernel stack.
5. ``sti`` — interrupts enabled during C handler.
6. Call ``syscall_dispatch(&args)``.
7. Store return value in frame; ``cli`` before return path.
8. Restore registers, ``SWAPGS``, ``sysret``.

Kernel stack layout
-------------------

The entry stub builds a ``struct syscall_frame`` on the task's kernel
stack:

.. code-block:: c

   struct syscall_frame {
       struct syscall_args {
           uint64_t nr;
           uint64_t arg0;  /* RDI */
           uint64_t arg1;  /* RSI */
           uint64_t arg2;  /* RDX */
           uint64_t arg3;  /* R10 */
           uint64_t arg4;  /* R8  */
           uint64_t arg5;  /* R9  */
       } args;

       struct syscall_user_state {
           uint64_t rflags;  /* saved R11 */
           uint64_t rip;     /* saved RCX */
           uint64_t rsp;     /* saved user RSP from GS slot 0 */
           uint64_t r12;
           uint64_t rbx;
           uint64_t rbp;
           uint64_t r13;
           uint64_t r14;
           uint64_t r15;
       } user;
   };

``syscall_user_state_of(args)`` returns a pointer to the ``user`` sub-struct
immediately following ``args`` on the stack. ``sys_fork()`` and
``sys_clone()`` copy this frame to forge the child's first userspace
return (``RAX = 0``). Fork keeps the parent's ``RSP``; clone overrides
``rsp`` with ``child_stack``.

``SWAPGS`` and user RSP
-----------------------

The stub cannot push user ``RSP`` onto the kernel stack before knowing the
kernel ``RSP``. GS slot 0 holds the user stack pointer across the
transition; the kernel saves it there before returning to userspace.

On ``SYSRET``, user ``RFLAGS`` comes from saved ``R11``; user ``RIP``
from ``RCX``.

.. warning::

   ``SYSRET`` to a non-canonical ``RIP`` raises ``#GP`` in ring 0.
   Validate userspace entry points during ELF load and after ``execve``.

Dispatcher
----------

``syscall_dispatch()`` in ``kernel/syscall/dispatch.c`` indexes a sparse
table by Linux syscall number. See :doc:`/syscall/interface` and
:doc:`/syscall/table`.

Path strings use ``syscall_copy_path()`` → ``copy_string_from_user()``
with ``JNU_PATH_MAX`` (256 bytes).

First userspace entry (contrast)
--------------------------------

The **first** ring-3 entry for a new process uses ``usermode_enter()``
in ``kernel/arch/x86_64/usermode.c`` (``iretq`` with constructed trap
frame), not ``SYSRET``. **Cloned threads** use ``usermode_enter_fork_frame()``
from ``thread_user_entry`` with a per-task forged frame (``rsp =
child_stack``, ``rax = 0``). Subsequent kernel entry from that process
uses ``SYSCALL`` after it invokes libc wrappers.

Return-to-user gates (G1 / G2)
------------------------------

Before restoring userspace at the end of a syscall (G1 in
``syscall_entry.S``) or an IRQ return path (G2 in ``isr.S``), the stub
calls ``arch_return_to_user_work()`` in ``kernel/kernel/retire.c``.
If ``signal_pending()`` (``TIF_NEED_DIE``) is set, the thread unwinds
through ``process_thread_exit()`` — the same path used by ``exit`` and
``exit_group`` teardown. Gates only run when returning to CPL 3, so
retirement never runs with kernel locks held from an interrupted kernel
context.

Blocking syscalls that must respond to group exit (e.g. ``wait4``) use
``sched_sleep_interruptible()`` and return ``-EINTR`` when
``TIF_NEED_DIE`` is pending, allowing the thread to reach a gate.
