System Call Interface
=====================

JNU implements a native system call ABI using the ``SYSCALL``/``SYSRET``
instruction pair on x86_64. The interface is not POSIX-compatible but
mirrors POSIX semantics where practical. All syscall numbers are defined
in ``include/jnu/syscall_nr.h``.

Calling Convention
------------------

See :doc:`/arch/syscall_entry` for the full register convention and stack
frame layout.

Return Values
-------------

All syscalls return a signed 64-bit integer (``int64_t``) in ``RAX``:

- A non-negative value indicates success. The meaning is syscall-specific.
- A negative value in the range ``[-4095, -1]`` indicates failure. The
  magnitude is the negated POSIX ``errno`` value (e.g., ``-ENOENT == -2``).

Pointer Validation
------------------

All userspace pointers passed to syscall handlers must be validated before
dereferencing. The helpers in ``usercopy.h`` centralize this:

.. code-block:: c

   bool user_range_ok(const void *uaddr, size_t len);

Returns ``true`` if the range ``[uaddr, uaddr+len)`` lies entirely within
the user address space (below ``USER_TOP = 0x0000800000000000``).

.. code-block:: c

   int user_range_mapped(const void *uaddr, size_t len, bool write);

Walks the page table of the current process to verify that every page in
the range is present, user-accessible (``PTE_USER``), and if ``write`` is
true, writable (``PTE_WRITE``). Returns 0 if all pages pass, ``-EFAULT``
otherwise.

.. code-block:: c

   int copy_from_user(void *dst, const void *usrc, size_t len);
   int copy_to_user(void *udst, const void *src, size_t len);

Safe copying primitives. Both call ``user_range_mapped()`` before
performing the copy. Return 0 on success, ``-EFAULT`` if the range is
invalid or inaccessible.

.. code-block:: c

   int copy_string_from_user(char *dst, const char *usrc, size_t max);

Copies a NUL-terminated string from user space into ``dst``, stopping at
``max`` bytes. Returns ``-EFAULT`` on an invalid pointer, ``-ENAMETOOLONG``
if no NUL terminator is found within ``max`` bytes.

.. warning::

   Syscall handlers must never dereference a user pointer directly. All
   paths must go through the helpers above. Violating this rule will
   trigger a kernel ``#PF`` when SMAP is enabled.

Dispatcher
----------

``syscall_dispatch(const struct syscall_args *args)`` is the central C
entry point called by the assembly stub. It:

1. Extracts the syscall number from ``args->nr``.
2. Bounds-checks against ``JNU_SYS_MAX``.
3. Dispatches to the handler function via a static jump table.
4. Returns ``-ENOSYS`` for unknown numbers.
