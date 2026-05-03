Initramfs
=========

The initramfs is a mandatory bootstrap filesystem delivered to the kernel
as a Limine module. It provides the initial userspace binaries (``/init``,
test programs, etc.) before the VFS and ATA subsystems are fully available.

Format
------

The initramfs uses the **CPIO newc** archive format (magic ``070701``).
Each entry in the archive is described by a 110-byte fixed-width ASCII
header followed by the file name (padded to a 4-byte boundary) and the
file data (padded to a 4-byte boundary). The archive is terminated by an
entry named ``TRAILER!!!``.

The following constants are defined in ``cpio_newc.h``:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Constant
     - Value / Meaning
   * - ``CPIO_NEWC_MAGIC``
     - ``"070701"`` — the six-byte magic string at the start of each header.
   * - ``CPIO_NEWC_HEADER_SIZE``
     - 110 — fixed size of the ASCII header in bytes.
   * - ``CPIO_NEWC_TRAILER``
     - ``"TRAILER!!!"`` — sentinel entry name that terminates the archive.
   * - ``CPIO_MODE_TYPE_MASK``
     - ``0170000`` — mask for the file-type bits in the mode field.
   * - ``CPIO_MODE_REG``
     - ``0100000`` — regular file.
   * - ``CPIO_MODE_DIR``
     - ``0040000`` — directory.

CPIO Parser
-----------

The low-level parser operates one entry at a time and is deliberately
minimal:

.. code-block:: c

   struct cpio_newc_entry {
       const char *name;
       size_t      name_len;
       const void *data;
       size_t      data_len;
       uint32_t    mode;
       size_t      next_off;  /* byte offset of the next entry in the archive */
   };

   int cpio_newc_next(const void *archive, size_t archive_len,
                      size_t off, struct cpio_newc_entry *out);

Parses the entry at byte offset ``off`` within the archive. Validates the
magic string, computes the data pointer as a direct reference into the
mapped archive (no copy), and fills ``next_off`` with the offset of the
following entry. Returns 0 on success, a negative errno on parse error, or
1 when the ``TRAILER!!!`` sentinel is reached.

The parser does not allocate memory. All string and data pointers in the
returned ``cpio_newc_entry`` point directly into the in-memory archive
image, which remains mapped for the lifetime of the kernel.

Initramfs Layer
---------------

The initramfs layer (``initramfs.h``) wraps the CPIO parser with a simple
lookup interface:

.. code-block:: c

   struct initramfs_file {
       const char *name;
       size_t      name_len;
       const void *data;
       size_t      size;
       uint32_t    mode;
   };

.. code-block:: c

   int  initramfs_init(void *base, size_t len);

Scans the entire CPIO archive at ``base``, building an internal index of
all regular files. Returns 0 on success or a negative errno if the archive
is malformed.

.. code-block:: c

   bool initramfs_ready(void);

Returns ``true`` if ``initramfs_init()`` has been called successfully.
Used by the exec adapter to decide whether the initramfs is available.

.. code-block:: c

   int initramfs_lookup(const char *path, struct initramfs_file *out);

Searches the internal index for a file whose name matches ``path``.
Returns 0 and fills ``out`` on success, or ``-ENOENT`` if not found.
Path matching is exact (no wildcards, no ``..`` resolution).

.. code-block:: c

   ssize_t initramfs_read_at(const struct initramfs_file *file,
                              uint64_t off, void *buf, size_t len);

Copies ``len`` bytes from ``file->data + off`` into ``buf``. This is the
``read_at`` callback registered in the ``exec_image`` structure when loading
an ELF from the initramfs. Returns the number of bytes copied, or 0 at EOF.

Relationship to the File Descriptor Layer
-----------------------------------------

Files opened from the initramfs via ``sys_open()`` are backed by
``JNU_FILE_INITRAMFS`` file descriptions. The ``struct initramfs_file`` is
embedded directly in ``struct file`` (in the ``u.initramfs`` union). The
current file offset is tracked in ``struct file::offset`` and advanced
on each ``read()`` call.

.. note::

   The initramfs does not support ``write()``, ``mkdir()``, or any
   mutating operation. It is a pure read-only bootstrap archive.
