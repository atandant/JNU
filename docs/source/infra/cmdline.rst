Command Line Parser
===================

The kernel command line is a single string passed by the Limine bootloader
through the ``limine_kernel_file`` response. It is parsed once during early
boot into a flat key-value table that subsystems query at runtime.

Format
------

The command line is a space-separated list of tokens. Each token is one of:

- ``key=value`` — sets the key to the given value string.
- ``key`` — sets the key to the implicit value ``"1"``.

Token parsing stops at ``CMDLINE_MAX_ENTRIES`` (32) entries. Keys are
truncated to ``CMDLINE_MAX_KEY`` (32) characters; values are truncated to
``CMDLINE_MAX_VALUE`` (64) characters. Excess tokens are silently ignored.

API
---

.. code-block:: c

   void cmdline_parse(const char *s);

Parses the command-line string ``s`` and populates the internal table.
Must be called once during ``kernel_main()``, before any subsystem calls
``cmdline_get()`` or ``cmdline_bool()``. The function is idempotent: if
called more than once, later calls overwrite the existing table.

.. code-block:: c

   const char *cmdline_get(const char *key);

Returns a pointer to the value string for ``key``, or ``NULL`` if the key
is not present. Bare keys return ``"1"``. The returned pointer is valid
for the lifetime of the kernel.

.. code-block:: c

   bool cmdline_bool(const char *key);

A convenience wrapper around ``cmdline_get()``. Returns ``true`` if the
key is present and its value is not the string ``"0"``. Returns ``false``
if the key is absent or its value is ``"0"``.

Known Keys
----------

The following keys are recognized by the kernel. Unrecognized keys are
stored verbatim and are accessible by drivers or future subsystems.

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Key
     - Description
   * - ``init=<path>``
     - Overrides the default init path (``/init``). Queried by
       ``start_init()`` in ``kernel_main()``.
   * - ``noinit=1``
     - Disables userspace launch. The kernel enters its idle loop after
       completing all initialization steps.
   * - ``selftest=1``
     - Enables the boot-time selftest suite. See :doc:`/infra/selftest`.
   * - ``panictest=1``
     - Unconditionally triggers a panic after init launch to test the
       panic output path. For development use only.
   * - ``execprobe=1``
     - Runs ``run_exec_probes()``, which validates the init binary without
       executing it. Useful for diagnosing ELF loader issues.
   * - ``dump=mem``
     - Calls ``pmm_dump()`` to print the physical memory map after the
       PMM is initialized.
   * - ``dump=blocks``
     - Hex-dumps the first 8 sectors of ``hda`` to the kernel log after
       the ATA driver is initialized.
