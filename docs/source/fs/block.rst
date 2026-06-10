Block Device Layer
==================

The block device abstraction decouples filesystem drivers from hardware.
Any driver that exposes ``struct block_ops`` can register a named device
(``hda``, ``vda``, …) for ``vfs_mount()`` and debugging.

Source: ``kernel/fs/block.c``, ``include/jnu/fs/block.h``.

Data structures
---------------

.. code-block:: c

   struct block_ops {
       int (*read) (struct block_device *bdev, uint64_t lba,
                    size_t count, void *buf);
       int (*write)(struct block_device *bdev, uint64_t lba,
                    size_t count, const void *buf);
   };

   struct block_device {
       const char          *name;
       uint32_t             sector_size;  /* 512 for all current drivers */
       uint64_t             sector_count;
       const struct block_ops *ops;
       void                *priv;
   };

All I/O is in **sectors** (512 bytes). Multi-sector reads/writes are
implemented by looping in the driver or Minix buffer layer.

Registry API
------------

.. code-block:: c

   int block_register(struct block_device *bdev);

Insert into a fixed-capacity global table. Caller retains ownership of
``bdev`` memory. Returns ``-ENOMEM`` if full, ``-EEXIST`` on name clash.

.. code-block:: c

   struct block_device *block_lookup(const char *name);

Lookup by name for mount and ``dump=blocks`` debugging.

.. code-block:: c

   int block_read(struct block_device *bdev, uint64_t lba,
                  size_t count, void *buf);

Thin wrapper around ``bdev->ops->read``.

Boot-time registration order
----------------------------

In ``kernel_main()`` after PCI init:

1. ``virtio_blk_init()`` — probes VirtIO PCI devices, registers ``vda``.
2. ``ata_init()`` — legacy IDE primary master/slave as ``hda``, ``hdb``, …

``vfs_mount()`` prefers ``vda`` when present (faster under QEMU virtio),
else ``hda``. See :doc:`/build` for ``make run-virtio``.

ATA driver (legacy IDE)
-----------------------

``kernel/drivers/ata.c`` — PIO mode, one sector per command.

* Primary channel I/O base ``0x1F0`` (master = ``hda``).
* Used by QEMU ``piix3-ide`` and VMware when the disk is attached as IDE
  0:0.
* Read and write supported; root Minix FS depends on correct disk image
  (``make ata-disk`` with ``mkfs.minix``).

VirtIO block driver
-------------------

``kernel/drivers/virtio_blk.c`` — PCI IDs ``0x1AF4:0x1042`` (modern) or
``0x1AF4:0x1001`` (transitional).

* Single polled virtqueue.
* DMA buffers in ``ZONE_DMA`` (bounce buffers for guest physical addresses).
* Write path issues ``VIRTIO_BLK_T_FLUSH`` after data writes.
* Registers device as ``vda``.

When both ``vda`` and ``hda`` exist, root mounts from ``vda`` first.

MINIX on-disk layout (summary)
------------------------------

For debugging mount failures:

* Bytes 1024–1025 on disk: Minix v1 magic ``0x137F`` (little-endian
  ``7F 13`` at offset ``0x410`` within sector 2).
* Superblock, inode map, zone map, and inodes follow classic Minix v1
  layout consumed by ``kernel/fs/minix/``.

Use ``dump=blocks`` on the kernel cmdline to hex-dump the first sectors of
``hda`` (see :doc:`/infra/cmdline`).

Related docs
------------

* VFS mount: :doc:`vfs`
* Disk image creation: :doc:`/build`
* PCI enumeration: ``kernel/drivers/pci.c`` (no dedicated doc page yet)
