Block Device Layer
==================

The block device abstraction decouples filesystem drivers from hardware
device drivers. Any driver that can supply a ``struct block_ops`` table
can register itself as a block device and be mounted as a filesystem.

Data Structures
---------------

.. code-block:: c

   struct block_ops {
       int (*read) (struct block_device *bdev, uint64_t lba,
                    size_t count, void *buf);
       int (*write)(struct block_device *bdev, uint64_t lba,
                    size_t count, const void *buf);
   };

   struct block_device {
       const char          *name;         /* e.g. "hda" */
       uint32_t             sector_size;  /* typically 512 */
       uint64_t             sector_count;
       const struct block_ops *ops;
       void                *priv;         /* driver-private data */
   };

All I/O is in units of sectors. The sector size is fixed per device and is
recorded in ``sector_size``. In v0.0.2.2, all registered devices have a
512-byte sector size.

.. note::

   The ``write`` operation is a placeholder in v0.0.2.2. All registered
   drivers return ``-ENOSYS`` for write requests.

API
---

.. code-block:: c

   int block_register(struct block_device *bdev);

Registers ``bdev`` in the global block device registry. The registry stores
a pointer to the caller's structure; the caller retains ownership of the
memory. Returns 0 on success, ``-ENOMEM`` if the registry is full, or
``-EEXIST`` if a device with the same name is already registered.

.. code-block:: c

   struct block_device *block_lookup(const char *name);

Returns the registered device with the given name, or ``NULL`` if not
found. Used by ``vfs_mount()`` to locate the target device.

.. code-block:: c

   int block_read(struct block_device *bdev, uint64_t lba,
                  size_t count, void *buf);

Convenience wrapper that calls ``bdev->ops->read()``. Returns 0 on success
or a negative errno.

ATA Driver
----------

The ATA driver (``kernel/drivers/``) registers the primary ATA device as
``hda`` during ``ata_init()``. It uses PIO mode to transfer data one sector
at a time; no DMA is used in v0.0.2.2.
