Command Line Parser
===================

The kernel command line is a single string from Limine's
``limine_kernel_file`` response (see ``boot/limine.cfg`` ``CMDLINE=``).
Parsed once in early boot into a flat key/value table for runtime queries.

Source: ``kernel/kernel/cmdline.c``, ``include/jnu/kernel/cmdline.h``.

Format
------

Space-separated tokens:

* ``key=value`` — store value string.
* ``key`` alone — implicit value ``"1"``.

Limits:

* At most ``CMDLINE_MAX_ENTRIES`` (32) pairs; excess tokens dropped.
* Keys truncated to ``CMDLINE_MAX_KEY`` (32) chars.
* Values truncated to ``CMDLINE_MAX_VALUE`` (64) chars.

API
---

.. code-block:: c

   void cmdline_parse(const char *s);

Called from ``kernel_main()`` before subsystems read options. Re-parsing
overwrites the table.

.. code-block:: c

   const char *cmdline_get(const char *key);

Return value pointer (lifetime = entire kernel) or ``NULL`` if absent.
Bare keys return ``"1"``.

.. code-block:: c

   bool cmdline_bool(const char *key);

``true`` if key present and value is not ``"0"``.

Known keys
----------

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - Key
     - Effect
   * - ``init=<path>``
     - Init executable path (default ``/init``). Loaded from initramfs
       first, then VFS. See :doc:`/proc/process`.
   * - ``noinit=1``
     - Skip ``start_init()``; kernel enters idle loop after init.
   * - ``selftest=1``
     - Run ``selftest_run_all()`` before userspace; panic on failure.
       See :doc:`selftest`.
   * - ``panictest=1``
     - Deliberate ``panic()`` after init for panic-path testing.
   * - ``execprobe=1``
     - Validate ELF headers for probe paths without executing.
   * - ``dump=mem``
     - After PMM init, print physical memory map via ``pmm_dump()``.
   * - ``dump=blocks``
     - After ATA init, hex-dump first 8 sectors of ``hda``. Useful when
       Minix mount fails (wrong/empty disk). See :doc:`/build`.
   * - ``kbd=kernel``
     - Keep keyboard echo in kernel idle loop instead of userspace only.
   * - ``numlock=on``
     - Force Num Lock on at boot (numpad digits). Default for MF2
       keyboards identified via PS/2 ``0xF2`` probe.
   * - ``numlock=off``
     - Force Num Lock off at boot (numpad keys act as navigation).

Limine configuration example
----------------------------

From ``boot/limine.cfg``:

.. code-block:: text

   TIMEOUT=3
   DEFAULT_ENTRY=1

   :JNU
   PROTOCOL=limine
   KERNEL_PATH=boot:///kernel.elf
   MODULE_PATH=boot:///initramfs.cpio
   MODULE_CMDLINE=initramfs
   CMDLINE=loglevel=3

   :JNU Selftest
   ...
   CMDLINE=loglevel=4 selftest=1

   :JNU (keyboard debug)
   ...
   CMDLINE=loglevel=4 kbd=kernel numlock=on

The initramfs module **must** have ``MODULE_CMDLINE=initramfs`` (or be
the sole module) so ``find_initramfs_module()`` locates the CPIO archive.
See :doc:`/arch/boot`.

Unrecognized keys
-----------------

Stored in the table and available via ``cmdline_get()`` for drivers or
future features. No error is raised for unknown keys.

Related docs
------------

* Boot sequence and cmdline hooks: :doc:`/arch/boot`
* VMware disk debugging: :doc:`/build`
