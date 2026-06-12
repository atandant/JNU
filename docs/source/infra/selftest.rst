Boot-Time Selftests
===================

The selftest framework provides a lightweight, opt-in mechanism to verify
kernel subsystems during boot. Selftests are gated on the ``selftest=1``
command-line option and run synchronously in ``kernel_main()`` before the
init process is launched.

Framework
---------

Each subsystem that supports selftests exports a function with the
signature ``int <subsystem>_selftest(void)``. The function returns 0 on
success or a negative errno on the first detected failure.

The selftest runner is a statically allocated table of ``struct selftest``
entries compiled into ``kernel/kernel/selftest.c``:

.. code-block:: c

   struct selftest {
       const char *name;
       int       (*run)(void);
   };

.. code-block:: c

   int selftest_run_all(void);

Iterates the table, calls each ``run`` function, and logs the result via
``pr_info`` (pass) or ``pr_err`` (fail). Returns the total number of
failures. ``kernel_main()`` calls ``panic()`` if the return value is
non-zero, preventing a corrupted kernel from attempting to run userspace.

Registered Selftests
--------------------

The following subsystems register selftests:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Function
     - What it tests
   * - ``pmm_selftest()``
     - Buddy allocator alloc/free round-trips; coalescing; zone boundaries.
   * - ``slab_selftest()``
     - Cache creation; object alloc/free across multiple slab pages;
       alloc/free count balance.
   * - ``vmm_selftest()``
     - ``vmm_map()``, ``vmm_protect()``, ``vmm_unmap()`` on a scratch
       address space.
   * - ``clone_space_selftest()``
     - CoW clone: parent and child share pages; first write triggers copy;
       parent is unaffected.
   * - ``rbtree_selftest()``
     - Insert, in-order traversal, erase, re-traversal.
   * - ``spinlock_selftest()``
     - Nested acquire/release; RFLAGS save/restore verification.
   * - ``vfs_selftest()``
     - Open, read, and close a file from the mounted Minix filesystem.
   * - ``initramfs_selftest()``
     - Lookup and read a known file from the initramfs archive.
   * - ``cpio_newc_selftest()``
     - CPIO header parsing against a small embedded test archive.
   * - ``sched_selftest()``
     - Kernel thread creation; yield; zombie reaping.
   * - ``mutex_selftest()``
     - In-kernel spinlock mutex acquire/release and contention paths.
   * - ``futex_selftest()``
     - ``futex_wake`` / ``futex_wait`` non-blocking paths and timed
       timeout on a scratch user mapping. See :doc:`/proc/futex`.
   * - ``process_selftest()``
     - Process creation; fork; wait; exit status propagation.
   * - ``syscall_selftest()``
     - Dispatcher bounds check; ``-ENOSYS`` for out-of-range numbers.
   * - ``usercopy_selftest()``
     - ``user_range_ok()`` boundary conditions; ``copy_from_user()``
       with valid and invalid pointers.
   * - ``file_refcount_selftest()``
     - ``file_get()`` / ``file_put()`` lifecycle; destruction on last put.

Adding a New Selftest
---------------------

1. Implement ``int mysubsystem_selftest(void)`` in the subsystem source
   file. Return 0 on success, negative errno on failure. Log details
   with ``pr_err`` before returning the failure code.
2. Declare the function in the subsystem's public header.
3. Add an entry to the table in ``kernel/kernel/selftest.c``.

Selftests should be entirely self-contained. They must not depend on
userspace, must not modify persistent kernel state that would affect the
subsequent init launch, and must clean up any resources they allocate.
