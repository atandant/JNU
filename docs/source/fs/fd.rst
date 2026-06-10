File Descriptor Table
=====================

The file descriptor table maps small non-negative integers to *open file
descriptions* (``struct file``). Multiple fds — even across processes
after ``fork()`` — may reference the same ``struct file`` when refcount
exceeds 1.

Source: ``kernel/user/fd.c``, ``include/jnu/user/fd.h``.

Data structures
---------------

.. code-block:: c

   struct file {
       enum jnu_file_type  type;       /* INITRAMFS, VFS, or CHARDEV */
       uint64_t            offset;     /* Current file position */
       uint32_t            flags;      /* O_RDONLY, O_WRONLY, O_RDWR, … */
       int                 refcount;
       union {
           struct initramfs_file  initramfs;
           struct vfs_inode      *vfs;
           struct char_device    *chardev;
       } u;
   };

   struct fd_table {
       struct file *slots[JNU_MAX_FDS];  /* JNU_MAX_FDS = 32 */
   };

Slots are indexed directly by fd number. ``NULL`` means unused. Maximum 32
open files per process.

Standard fds
------------

Nothing in the kernel auto-opens stdin/stdout/stderr. Init and test
programs open ``/dev/serial`` or rely on musl's default fd 1/2 behavior
after explicit open. Musl may write to fd 1 before a console device is
opened — JNU does not synthesize fd 0–2 at process creation.

Backing types
-------------

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Type
     - Behavior
   * - ``JNU_FILE_INITRAMFS``
     - Read-only CPIO entry; seek supported. Used for ``/init`` and
       binaries only in initramfs.
   * - ``JNU_FILE_VFS``
     - Minix inode via VFS; read/write per open flags and inode type.
   * - ``JNU_FILE_CHARDEV``
     - ``/dev/kbd`` — keyboard scancodes; ``/dev/serial`` — write-only
       mirror of COM1 for userspace logging.

``open`` path resolution (``sys_open``) tries initramfs, then VFS root,
then well-known chardev paths.

Reference counting
------------------

Every ``struct file`` starts with ``refcount = 1``.

1. ``fd_alloc()`` — places file in lowest free slot; does not bump refcount
   (slot holds the initial reference).
2. ``fd_close()`` — ``file_put()`` on slot contents; clears slot.
3. ``fd_table_clone()`` (fork) — ``file_get()`` on each populated slot
   before copying pointer.
4. ``file_get()`` / ``file_put()`` — increment/decrement; at zero, type-
   specific cleanup (``vfs_close``, etc.) and ``kfree`` of ``struct file``.

.. warning::

   ``refcount`` is a plain ``int``. Safe on single-CPU with fork dup under
   preemption disable. SMP needs atomics.

API
---

.. code-block:: c

   void fd_table_init(struct fd_table *table);
   void fd_table_clone(struct fd_table *dst, struct fd_table *src);
   int fd_alloc(struct fd_table *table, struct file *file);
   struct file *fd_get(struct fd_table *table, int fd);
   struct file *fd_close(struct fd_table *table, int fd);

``fd_get`` does not change refcount; callers must not use the pointer
after dropping locks if another thread might ``close`` the fd (future SMP).

Syscall usage pattern
---------------------

Typical handler sequence:

1. ``fd = arg0``; reject ``fd < 0`` → ``-EBADF``.
2. ``file = fd_get(&proc->fds, fd)``; NULL → ``-EBADF``.
3. Dispatch on ``file->type``; validate user buffers with usercopy helpers.
4. Update ``file->offset`` on read/write/lseek as appropriate.

``writev`` iterates iovec entries in user memory with per-chunk validation.

There is no ``dup`` or ``fcntl`` syscall in v0.0.3; musl rarely needs them
for the supported test programs.

Selftests
---------

``file_refcount_selftest()`` verifies get/put lifecycle when
``selftest=1``. See :doc:`/infra/selftest`.

Related docs
------------

* VFS and open path: :doc:`vfs`
* Initramfs backing: :doc:`initramfs`
* Syscall list: :doc:`/syscall/table`
