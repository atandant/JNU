Build and setup
===============

JNU uses the top-level ``Makefile`` as the only supported build entry
point. Helper scripts under ``scripts/`` are implementation details for
Make targets.

Standard workflow
-----------------

Run these commands from the repository root:

.. code-block:: sh

   make doctor
   make bootstrap-limine
   make
   make run

``make`` builds native JNU userspace, the kernel ELF, the initramfs, and
``build/kernel.iso``. It does not require musl.

Required tools
--------------

The normal ISO build requires:

* ``clang``
* ``ld.lld``
* ``nasm``
* ``make``
* ``python3``
* ``xorriso``
* ``git`` for bootstrapping Limine
* Limine checked out and built in ``boot/limine``

On Debian or Ubuntu hosts, install the core toolchain with:

.. code-block:: sh

   sudo apt install clang lld nasm make xorriso git mtools

Optional tools
--------------

``make run`` requires ``qemu-system-x86_64``. ``make ata-disk`` and
``make run-disk`` use ``mkfs.minix`` when available to create a populated
MINIX v1 disk image; install ``util-linux`` on Debian/Ubuntu for that
tool.

Sphinx is only needed for ``make docs``. Musl is only needed for
``make musltest`` or ``make iso-musl``.

Targets
-------

``make doctor``
   Check required tools and report optional tools.

``make bootstrap-limine``
   Clone and build Limine under ``boot/limine``.

``make`` / ``make iso``
   Build ``build/kernel.iso`` with native userspace.

``make user``
   Build native userspace programs discovered as ``user/*/main.c``.

``make musltest``
   Build the optional musl-linked test program. This expects musl to be
   installed under ``user/musl/install``.

``make iso-musl``
   Build ``build/kernel-musl.iso`` including ``musltest``.

``make ata-disk``
   Create ``build/disk.img``. Set ``SIZE=N`` to choose MiB size.

``make run`` / ``make run-disk``
   Boot in QEMU with a legacy IDE disk (``hda``). ``make run`` attaches
   ``build/disk.img`` if it already exists; ``make run-disk`` creates/requires
   it.

``make run-virtio``
   Boot in QEMU with a ``virtio-blk-pci`` disk (``vda``). Requires
   ``build/disk.img``.

``make debug`` / ``make debug-disk``
   Boot QEMU paused with ``-s -S`` for GDB.

``make docs``
   Build HTML docs with Sphinx.

``make clean`` / ``make distclean``
   Remove generated outputs. ``distclean`` also removes Limine.

``make vmware-disk``
   Build ``build/disk.img`` and convert it to ``build/disk.vmdk`` for
   VMware. Requires ``qemu-img``.

Running on VMware
-----------------

JNU mounts a raw MINIX v1 filesystem from the legacy IDE primary master
(``hda`` on I/O port ``0x1F0``). QEMU attaches ``build/disk.img`` the
same way via ``piix3-ide``.

A blank disk created by the VMware new-VM wizard has **no MINIX
filesystem**. The kernel will panic at boot with ``invalid magic
0x0000`` because the superblock at byte offset 1024 is zeroed. Guest
RAM size (for example 512 MiB) does not cause this failure.

**1. Build the disk image on the host**

.. code-block:: sh

   make ata-disk
   make doctor    # confirms mkfs.minix is installed

``make ata-disk`` must report that ``mkfs.minix`` laid a MINIX v1
filesystem. If util-linux is missing, the image contains only a test
signature and mount will still fail.

**2. Prepare a VMware-compatible disk (optional)**

.. code-block:: sh

   make vmware-disk

This runs ``make ata-disk`` and converts ``build/disk.img`` to
``build/disk.vmdk``. You can also convert manually:

.. code-block:: sh

   qemu-img convert -f raw -O vmdk build/disk.img build/disk.vmdk

**3. Attach the disk in VMware**

Configure the virtual machine as follows:

* **Disk bus:** IDE (not NVMe, SCSI, or SATA/AHCI).
* **Position:** IDE 0:0 (primary master).
* **Disk file:** ``build/disk.img`` (flat/raw) or ``build/disk.vmdk``.

Remove or replace any blank wizard-created VMDK so ``hda`` is the MINIX
image, not an empty virtual disk.

**4. Boot the ISO and check the serial log**

Expected lines before userspace starts:

.. code-block:: text

   ata: hda: '<model>' N sectors (...)
   rootfs: N entries

If mount still fails, set the Limine cmdline in ``boot/limine.cfg`` to
``loglevel=4 dump=blocks``. Sector 2 should show ``7f 13`` at offset
``0x10`` (MINIX v1 magic ``0x137F`` at disk byte 1040). All zeros mean
the wrong or empty disk is still attached.

Running with virtio-blk in QEMU
-------------------------------

For faster block I/O under QEMU, use the virtio-blk driver instead of
legacy IDE:

.. code-block:: sh

   make ata-disk
   make run-virtio

Or pass ``--disk-type virtio`` to ``scripts/run-qemu.sh``. When both
``vda`` (virtio) and ``hda`` (ATA) are present, root mounts from ``vda``
first.
