System Call Interface
=====================

JNU implements the x86_64 ``SYSCALL``/``SYSRET`` fast path for
userspace/kernel transitions. Since v0.0.3, **syscall numbers follow the
Linux x86_64 ABI** (see ``include/uapi/jnu/syscall_nr.h``) so statically
linked musl binaries work without patching musl itself. Register layout,
error encoding, and per-syscall semantics remain JNU's.

The full trap path — MSR setup, assembly stub, stack frame — is
documented in :doc:`/arch/syscall_entry`. The complete syscall list is
in :doc:`table`.

Calling convention
------------------

Userspace places arguments per the Linux x86_64 syscall ABI:

.. list-table::
   :header-rows: 1
   :widths: 15 85

   * - Register
     - Role
   * - ``RAX``
     - Syscall number
   * - ``RDI``
     - Argument 0
   * - ``RSI``
     - Argument 1
   * - ``RDX``
     - Argument 2
   * - ``R10``
     - Argument 3 (``RCX`` is clobbered by ``SYSCALL``)
   * - ``R8``
     - Argument 4
   * - ``R9``
     - Argument 5

Native programs call through ``user/libjnu/syscall.S``; musl uses its
own arch stub with the same register assignment.

Return values
-------------

All syscalls return a signed 64-bit integer in ``RAX``:

* **Success:** non-negative value (meaning is syscall-specific; e.g. byte
  count for ``read``/``write``, fd for ``open``, 0 for many mutators).
* **Failure:** negative value in ``[-4095, -1]``. The magnitude is the
  negated POSIX ``errno`` (e.g. ``-ENOENT == -2``, ``-EINVAL == -22``).

Syscalls that do not return on success (``exit``, ``execve``) never
restore userspace; they terminate or replace the process image in kernel
context.

Pointer validation
------------------

Every userspace pointer must be validated before dereference. Helpers
live in ``kernel/user/copy.c`` and ``include/jnu/user/usercopy.h``:

.. code-block:: c

   bool user_range_ok(const void *uaddr, size_t len);

Returns ``true`` if ``[uaddr, uaddr+len)`` lies entirely below
``USER_TOP`` (``0x0000800000000000``).

.. code-block:: c

   int user_range_mapped(const void *uaddr, size_t len, bool write);

Walks the current process page tables. Verifies each page is present,
user-accessible (``PTE_USER``), and writable when ``write`` is true.
Returns 0 or ``-EFAULT``.

.. code-block:: c

   int copy_from_user(void *dst, const void *usrc, size_t len);
   int copy_to_user(void *udst, const void *src, size_t len);

Copy after ``user_range_mapped()`` check. Return 0 or ``-EFAULT``.

.. code-block:: c

   int copy_string_from_user(char *dst, const char *usrc, size_t max);

Copy a NUL-terminated path or name. Returns ``-EFAULT`` or
``-ENAMETOOLONG`` on failure.

Path arguments longer than ``JNU_PATH_MAX`` (256 bytes) are rejected at
``syscall_copy_path()`` in ``kernel/syscall/common.c``.

.. warning::

   Direct kernel dereference of user pointers violates SMAP when enabled
   and will ``#PF`` in ring 0. Always use the helpers above.

Dispatcher
----------

``syscall_dispatch(const struct syscall_args *args)`` in
``kernel/syscall/dispatch.c`` is the C entry point from
``syscall_entry.S``:

1. Records ``args->nr`` in per-CPU syscall scratch (for debugging).
2. If ``args->nr <= JNU_SYS_MAX`` and the sparse table slot is non-NULL,
   calls the handler wrapper.
3. Otherwise returns ``-ENOSYS``.
4. Clears the recorded syscall number and returns the handler result.

Handlers run with interrupts enabled (``sti`` in the entry stub) after
the kernel stack is established. They must not block indefinitely without
a plan to yield or sleep — the single-CPU scheduler can starve other
tasks.

Reentrancy and locking
----------------------

Syscall handlers typically:

* Resolve ``sched_current()->process`` for per-process state.
* Use ``fd_get()`` / ``fd_alloc()`` on the process fd table (mutex for
  VFS inodes where needed).
* Call into VMM/PMM with existing MM locks.

Nested syscalls from the same task are possible if a page fault occurs
while copying user memory; the fault handler runs on the same kernel
stack. Handlers should avoid holding spinlocks across operations that
might fault on user addresses.

Stubs for musl compatibility
----------------------------

Several syscalls exist solely so musl's static startup does not abort:

* ``rt_sigaction`` / ``rt_sigprocmask`` — return 0 (no signals yet).
* ``ioctl`` — returns ``-ENOTTY`` (stdio terminal probes).
* ``set_tid_address`` — records address, returns TID.

Real signal delivery and terminal ioctls are future work. See
:doc:`table` for the full list.

Native vs musl userspace
------------------------

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - Component
     - Role
   * - ``user/libjnu/``
     - Minimal libc: crt0, syscall wrappers, open/read/write/fork/execve
   * - ``user/musl/``
     - Full musl tree (not vendored; clone locally). See :doc:`/musl`.
   * - ``include/uapi/jnu/``
     - Shared ABI headers; regenerated into ``user/libjnu/include/`` by
       ``scripts/gen-uapi.sh``

Both stacks use the same kernel syscall numbers and return convention.
