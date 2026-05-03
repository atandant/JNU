Kernel Log
==========

The kernel log (``klog``) provides the internal message infrastructure for
the JNU kernel. It exposes ``printk()`` as the formatting core and
``pr_*`` macros as the call-site API. Output is multiplexed across a set
of registered backends via a ring buffer.

Backends
--------

A backend is a ``struct klog_backend``:

.. code-block:: c

   struct klog_backend {
       const char          *name;
       uint32_t             flags;
       void               (*write)(const char *buf, size_t len);
       struct klog_backend *next;  /* intrusive linked list */
   };

The only defined flag is ``KLOG_BACKEND_ANSI`` (bit 0). Backends with this
flag receive ANSI color escape sequences for ``KLOG_ERR`` and ``KLOG_WARN``
levels; backends without it receive plain text.

In v0.0.2 two backends are registered:

- **COM1 serial** (``KLOG_BACKEND_ANSI`` clear): raw text to the UART.
- **Framebuffer console**: rendered via ``fbcon``; ANSI colors rendered
  as colored text when the framebuffer is available.

Log Levels
----------

.. list-table::
   :header-rows: 1
   :widths: 15 10 75

   * - Level
     - Value
     - Usage
   * - ``KLOG_PANIC``
     - 0
     - Imminent kernel termination. Used exclusively by ``panic()``.
   * - ``KLOG_ERR``
     - 1
     - Errors from which the kernel may recover, but that indicate
       a failed operation.
   * - ``KLOG_WARN``
     - 2
     - Unexpected but non-fatal conditions (e.g. missing optional hardware).
   * - ``KLOG_INFO``
     - 3
     - Informational messages emitted during normal operation.
   * - ``KLOG_DEBUG``
     - 4
     - High-verbosity messages; disabled in production builds.

API
---

.. code-block:: c

   void klog_init(void);

Initializes the ring buffer. Must be the first kernel call; no other
subsystem output is possible until this returns.

.. code-block:: c

   void klog_register(struct klog_backend *be);

Appends ``be`` to the backend list. The ring buffer is drained into the
newly registered backend immediately so that early messages (logged before
the backend was available) are not lost.

.. code-block:: c

   void printk(enum klog_level level, const char *fmt, ...);
   void vprintk(enum klog_level level, const char *fmt, __builtin_va_list ap);

The central formatting functions. ``printk`` prepends a timestamp in
``[secs.usecs]`` format and a level indicator, then writes to all backends.

``pr_*`` Macros
---------------

The following macros are the standard call-site API. They expand to
``printk()`` with the corresponding log level:

.. code-block:: c

   pr_panic(fmt, ...)  /* KLOG_PANIC */
   pr_err(fmt, ...)    /* KLOG_ERR   */
   pr_warn(fmt, ...)   /* KLOG_WARN  */
   pr_info(fmt, ...)   /* KLOG_INFO  */
   pr_debug(fmt, ...)  /* KLOG_DEBUG */

User Output Separation
----------------------

.. code-block:: c

   void klog_user_write(enum klog_level level, int pid,
                        const char *buf, size_t len);

Userspace ``write()`` calls to fd 1 (stdout) and fd 2 (stderr) are routed
to this function rather than ``printk()``. Each line is prefixed with
``user[pid=N]: `` to prevent spoofing. Control bytes other than ``\n`` and
``\t`` are replaced with ``.`` to prevent ANSI injection.

Panic Write Path
----------------

.. code-block:: c

   void klog_panic_write(const char *buf, size_t len);

Used exclusively by ``panic()``. Bypasses the ring buffer and all locks,
writing directly to every backend. This ensures that the panic message is
delivered even if the ring buffer lock is held by the panicking CPU.

.. code-block:: c

   void klog_drain_tail(size_t n, void (*cb)(const char *line, size_t len));

Drains the last ``n`` lines from the ring buffer and delivers each to
``cb``. Called by ``panic_with_state()`` to replay recent log history after
the register dump.
