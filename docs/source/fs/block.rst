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

   The ATA driver supports read and write via PIO. The virtio-blk driver
   supports read, write, and flush via a polled virtqueue.

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

The ATA driver (``kernel/drivers/ata.c``) registers legacy IDE devices as
``hda``, ``hdb``, and so on during ``ata_init()``. It uses PIO mode to
transfer data one sector at a time.

VirtIO Block Driver
-------------------

The virtio-blk driver (``kernel/drivers/virtio_blk.c``) probes PCI for
``0x1AF4:0x1042`` (modern) or ``0x1AF4:0x1001`` (transitional) and
registers the disk as ``vda``. It uses one polled virtqueue, DMA bounce
buffers in ``ZONE_DMA``, and acknowledges ``VIRTIO_F_VERSION_1`` on
transitional devices. Writes are followed by a ``VIRTIO_BLK_T_FLUSH``
request.

At boot, ``kernel_main`` mounts root from ``vda`` when present, otherwise
falls back to ``hda``.
