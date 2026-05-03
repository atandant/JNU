Syscall Entry and Frame Layout
==============================

The JNU native syscall ABI uses the ``SYSCALL``/``SYSRET`` instruction pair.
``SYSCALL`` saves ``RIP`` into ``RCX``, saves ``RFLAGS`` into ``R11``, and
transfers control to the address in ``IA32_LSTAR``.

Register Convention
-------------------

On entry to ``syscall_entry.S``, the register assignments are:

.. list-table::
   :header-rows: 1
   :widths: 15 85

   * - Register
     - Role
   * - ``RAX``
     - Syscall number (``JNU_SYS_*``).
   * - ``RDI``
     - Argument 0.
   * - ``RSI``
     - Argument 1.
   * - ``RDX``
     - Argument 2.
   * - ``R10``
     - Argument 3 (replaces ``RCX``, which is overwritten by ``SYSCALL``).
   * - ``R8``
     - Argument 4.
   * - ``R9``
     - Argument 5.
   * - ``RCX``
     - (Clobbered by ``SYSCALL``; holds user RIP on entry to the stub.)
   * - ``R11``
     - (Clobbered by ``SYSCALL``; holds user RFLAGS on entry to the stub.)

Kernel Stack Layout
-------------------

The entry stub builds a ``struct syscall_frame`` on the kernel stack. The
layout from lowest to highest address reflects the push order in
``syscall_entry.S``:

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
           uint64_t rip;     /* saved RCX (user return address) */
           uint64_t rsp;     /* saved user RSP from GS slot 0 */
           uint64_t r12;
           uint64_t rbx;
           uint64_t rbp;
           uint64_t r13;
           uint64_t r14;
           uint64_t r15;
       } user;
   };

The ``syscall_user_state_of(args)`` inline accessor returns a pointer to
the ``syscall_user_state`` that immediately follows a ``syscall_args`` on
the stack. This is used by ``sys_fork()`` to forge the child's return frame
without a separate allocation.

``SWAPGS`` and GS Slot Usage
-----------------------------

On entry, the stub executes ``SWAPGS`` to bring in the kernel GS base. GS
slot 0 holds the saved user RSP, which is saved there by the kernel before
returning to userspace. This avoids a chicken-and-egg problem: the stub
cannot save RSP to the kernel stack before knowing the kernel RSP.

On return, ``SWAPGS`` is executed again before ``SYSRET`` to restore user
GS. The user ``RFLAGS`` is restored from the saved ``R11``; ``SYSRET``
restores ``RIP`` from ``RCX``.

.. warning::

   ``SYSRET`` to a non-canonical ``RIP`` raises ``#GP`` in ring 0, not in
   ring 3. Userspace RIP values must be validated against the canonical
   address mask before returning to avoid an unrecoverable kernel exception.

Dispatcher
----------

The C dispatcher ``syscall_dispatch(const struct syscall_args *args)``
performs a bounds check against ``JNU_SYS_MAX`` and dispatches to the
appropriate handler via a jump table. Unknown syscall numbers return
``-ENOSYS``.

Path strings passed in userspace pointers are copied via
``syscall_copy_path(dst, upath)``, which internally calls
``copy_string_from_user()`` with a ``JNU_PATH_MAX`` (256 byte) limit.
