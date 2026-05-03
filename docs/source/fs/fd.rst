File Descriptor Table
=====================

The file descriptor table maps small non-negative integers (file
descriptors) to *open file descriptions* (``struct file``). Multiple file
descriptors — potentially in different processes — may share a single
open-file description when the description's reference count exceeds 1.

Data Structures
---------------

.. code-block:: c

   struct file {
       enum jnu_file_type  type;       /* INITRAMFS, VFS, or CHARDEV */
       uint64_t            offset;     /* Current file position */
       uint32_t            flags;      /* Open flags (O_RDONLY, etc.) */
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

The ``slots`` array is indexed directly by file descriptor number. An empty
slot is ``NULL``. The maximum number of open files per process is
``JNU_MAX_FDS`` (32).

Reference Counting Rules
------------------------

Every ``struct file`` is born with ``refcount = 1``. The following rules
govern its lifetime:

1. ``fd_alloc()`` inserts a file into the first free slot. It does **not**
   increment the refcount; the slot owns the initial reference.
2. ``fd_close()`` removes the slot's reference by calling ``file_put()``.
   If the refcount reaches 0, the file's backing resource is released.
3. ``fd_table_clone()`` (used by ``fork()``) calls ``file_get()`` on every
   populated slot before copying the pointer, so both the parent and child
   fd tables hold a reference.
4. ``file_get()`` increments the refcount.
5. ``file_put()`` decrements the refcount. When it reaches 0, the
   appropriate cleanup function is called (``vfs_close()``,
   ``initramfs_close()``, etc.) and the ``struct file`` is freed.

.. warning::

   In v0.0.2.1 the refcount is a plain ``int``. Access is safe only because
   the kernel is single-CPU and the ``fork()`` dup loop runs with
   preemption disabled. SMP requires converting ``refcount`` to an atomic.

API
---

.. code-block:: c

   void fd_table_init(struct fd_table *table);

Zeroes all slots in ``table``. Must be called before any other fd operation
on the table.

.. code-block:: c

   void fd_table_clone(struct fd_table *dst, struct fd_table *src);

Copies all populated slot pointers from ``src`` into ``dst``, calling
``file_get()`` on each. ``dst`` must have been freshly initialized.

.. code-block:: c

   int fd_alloc(struct fd_table *table, struct file *file);

Finds the lowest-numbered free slot in ``table``, stores ``file`` there,
and returns the slot index as the file descriptor. Returns ``-EMFILE`` if
all 32 slots are occupied.

.. code-block:: c

   struct file *fd_get(struct fd_table *table, int fd);

Returns the ``struct file *`` at slot ``fd``, or ``NULL`` if ``fd`` is out
of range or the slot is empty. Does not modify the refcount; the caller
must not hold the pointer beyond the current critical section without
calling ``file_get()``.

.. code-block:: c

   struct file *fd_close(struct fd_table *table, int fd);

Removes the file from slot ``fd``, calls ``file_put()``, and returns the
now-released pointer (for callers that need to inspect the file after
dropping the table's reference). Returns ``NULL`` if ``fd`` is invalid.
