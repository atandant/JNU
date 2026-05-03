Virtual File System
===================

The VFS provides a uniform file access interface over multiple backing
filesystems. In v0.0.2.2, the only registered filesystem type is Minix.
The VFS layer is read-only; write operations are not yet implemented.

Architecture
------------

The VFS is structured around three types:

- ``struct vfs_mount``: one per mounted filesystem. Holds a pointer to the
  backing block device, the filesystem operations table, and the root inode.
- ``struct vfs_inode``: one per open file or directory. Holds the inode
  number, size, mode, UID/GID, and a pointer back to the owning mount.
- ``struct vfs_ops``: a vtable of function pointers implementing the
  operations for a specific filesystem type.

Data Structures
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
       void   (*close)  (struct vfs_inode *ino);
   };

   struct vfs_mount {
       struct block_device     *bdev;
       const struct vfs_ops    *ops;
       void                    *priv;   /* filesystem-private state */
       struct vfs_inode        *root;
   };

   struct vfs_inode {
       struct vfs_mount *mnt;
       uint32_t          ino;
       uint64_t          size;
       bool              is_dir;
       uint16_t          mode;
       uint16_t          uid;
       uint16_t          gid;
       void             *priv;  /* filesystem-private inode state */
   };

   struct vfs_dirent {
       uint32_t ino;
       char     name[VFS_NAME_MAX];   /* VFS_NAME_MAX = 64 */
   };

API
---

.. code-block:: c

   void vfs_init(void);

Initializes the VFS subsystem. Must be called before ``vfs_mount()``.

.. code-block:: c

   int vfs_mount(const char *bdev_name, const char *fstype,
                 const char *target);

Mounts the filesystem of type ``fstype`` from the block device named
``bdev_name`` at the path ``target``. In v0.0.2.2 the only supported
``fstype`` is ``"minix"`` and the only supported ``target`` is ``"/"``.
Returns 0 or a negative errno.

.. code-block:: c

   int vfs_open(const char *path, struct vfs_inode **out);

Resolves ``path`` relative to the mounted root. Walks each path component
by calling ``ops->lookup()`` from the root inode. Returns 0 and sets
``*out`` to the resolved inode on success, or a negative errno if any
component is not found or the caller lacks permission.

.. code-block:: c

   ssize_t vfs_read(struct vfs_inode *ino, uint64_t offset,
                    size_t len, void *buf);

Reads ``len`` bytes from ``ino`` starting at ``offset`` into the kernel
buffer ``buf``. Returns the number of bytes read (may be less than ``len``
at EOF), or a negative errno.

.. code-block:: c

   int vfs_readdir(struct vfs_inode *dir, size_t index,
                   struct vfs_dirent *out);

Reads the directory entry at position ``index`` from ``dir``. Returns 1
if an entry was written to ``out``, 0 if the index is past the last entry,
or a negative errno on error.

.. code-block:: c

   void vfs_close(struct vfs_inode *ino);

Releases the inode. Calls ``ops->close()`` to allow the filesystem to
free any private state. The inode pointer must not be used after this call.

Path Resolution
---------------

The path resolver in ``vfs_open()`` is minimal: it tokenizes on ``/`` and
calls ``ops->lookup()`` once per component from the root. It does not
handle ``.`` or ``..``, symlinks, or mount-point traversal.

.. note::

   ``VFS_NAME_MAX`` is 64 bytes. Path components longer than 63 characters
   (plus NUL) will cause ``ops->lookup()`` to return ``-ENOENT``.

Integration with the File Descriptor Layer
------------------------------------------

Syscall handlers do not call the VFS directly. They interact with it
through the file descriptor layer (``fd.h``). ``sys_open()`` calls
``vfs_open()``, wraps the resulting inode in a ``struct file`` of type
``JNU_FILE_VFS``, and allocates a slot in the process fd table via
``fd_alloc()``.

Subsequent ``read()``, ``lseek()``, and ``fstat()`` calls retrieve the
``struct file`` via ``fd_get()`` and dispatch to ``vfs_read()`` or
equivalent.
