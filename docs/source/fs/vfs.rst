Virtual File System
===================

The VFS provides a uniform path and inode interface over concrete
filesystem implementations. In v0.0.3 the only registered type is **Minix
v1** on a block device; the ops table also supports write, create, and
unlink for musl filesystem tests.

Source: ``kernel/fs/vfs.c``, ``kernel/fs/minix/``, ``include/jnu/fs/vfs.h``.

Architecture
------------

Three core types:

* ``struct vfs_mount`` — one mounted instance (block device, ops vtable,
  filesystem-private ``priv``, root inode).
* ``struct vfs_inode`` — file or directory metadata (ino, size, mode,
  per-inode mutex).
* ``struct vfs_ops`` — filesystem callbacks (mount, lookup, read, write, …).

At boot, ``kernel_main()`` calls ``vfs_mount("vda", "minix", "/")`` or
falls back to ``hda``. Only a single root mount exists; there is no
mount-namespace or ``..`` traversal across mount points.

Data structures
---------------

.. code-block:: c

   struct vfs_ops {
       int    (*mount)  (struct vfs_mount *mnt, struct block_device *bdev);
       int    (*lookup) (struct vfs_inode *dir, const char *name,
                         struct vfs_inode **out);
       int    (*readdir)(struct vfs_inode *dir, size_t index,
                         struct vfs_dirent *out);
       ssize_t(*read)   (struct vfs_inode *ino, uint64_t offset,
                         size_t len, void *buf);
       ssize_t(*write)  (struct vfs_inode *ino, uint64_t offset,
                         size_t len, const void *buf);
       int    (*truncate)(struct vfs_inode *ino, uint64_t size);
       int    (*create) (struct vfs_inode *dir, const char *name,
                         uint16_t mode, struct vfs_inode **out);
       int    (*unlink) (struct vfs_inode *dir, const char *name);
       int    (*mkdir)  (struct vfs_inode *dir, const char *name,
                         uint16_t mode);
       int    (*rmdir)  (struct vfs_inode *dir, const char *name);
       int    (*rename) (struct vfs_inode *old_dir, const char *old_name,
                         struct vfs_inode *new_dir, const char *new_name);
       int    (*fsync)  (struct vfs_inode *ino);
       void   (*close)  (struct vfs_inode *ino);
   };

   struct vfs_mount {
       struct block_device     *bdev;
       const struct vfs_ops    *ops;
       void                    *priv;
       struct vfs_inode        *root;
   };

   struct vfs_inode {
       struct mutex      lock;
       struct vfs_mount *mnt;
       uint32_t          ino;
       uint64_t          size;
       bool              is_dir;
       uint16_t          mode;
       uint16_t          uid;
       uint16_t          gid;
       void             *priv;
   };

Mount and path operations
-------------------------

.. code-block:: c

   void vfs_init(void);
   int vfs_mount(const char *bdev_name, const char *fstype, const char *target);

``vfs_mount`` looks up the block device by name, calls ``ops->mount``,
and records the root inode. Supported: ``fstype == "minix"``,
``target == "/"``.

.. code-block:: c

   int vfs_open(const char *path, struct vfs_inode **out);

Tokenize ``path`` on ``/`` and walk from root via ``ops->lookup`` per
component. Does not resolve ``.`` or ``..`` or symlinks.

.. code-block:: c

   ssize_t vfs_read(struct vfs_inode *ino, uint64_t offset,
                    size_t len, void *buf);
   ssize_t vfs_write(struct vfs_inode *ino, uint64_t offset,
                     size_t len, const void *buf);

Read/write through inode mutex and Minix file ops.

.. code-block:: c

   int vfs_create(const char *path, uint16_t mode, struct vfs_inode **out);
   int vfs_unlink(const char *path);
   int vfs_mkdir(const char *path, uint16_t mode);
   int vfs_rmdir(const char *path);
   int vfs_rename(const char *old_path, const char *new_path);
   int vfs_truncate(struct vfs_inode *ino, uint64_t size);
   int vfs_fsync(struct vfs_inode *ino);

Mutators used by write-related syscalls (``creat``, ``unlink``, ``mkdir``,
etc.). All paths are relative to the single root mount.

.. code-block:: c

   int vfs_readdir(struct vfs_inode *dir, size_t index,
                   struct vfs_dirent *out);
   void vfs_close(struct vfs_inode *ino);

Path resolution limits
----------------------

* ``VFS_NAME_MAX`` is 64 bytes per path component.
* No symbolic links, permission checks against current uid, or multiple
  mount points.
* Longer component names fail lookup with ``-ENOENT``.

Integration with file descriptors
---------------------------------

Syscall handlers do not call VFS directly. Flow:

1. ``open`` → resolve path (initramfs, VFS, or chardev) → wrap in
   ``struct file`` → ``fd_alloc()``.
2. ``read``/``write``/``lseek``/``fstat`` → ``fd_get()`` → dispatch by
   ``file->type``.

See :doc:`fd` and :doc:`/syscall/table`.

Minix implementation
--------------------

Under ``kernel/fs/minix/``:

* Superblock and magic ``0x137F`` at byte offset 1024 on disk
* Inode and zone tables, bitmap allocation
* Buffer cache for block I/O via :doc:`block`

Wrong or blank disk images panic at mount with invalid magic — see
:doc:`/build` (VMware/QEMU disk setup).

Selftests
---------

``vfs_selftest()`` opens and reads a file from the mounted root when
``selftest=1``. Requires successful Minix mount on ``vda`` or ``hda``.
