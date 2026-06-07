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
   Boot in QEMU. ``make run`` attaches ``build/disk.img`` if it already
   exists; ``make run-disk`` creates/requires it.

``make debug`` / ``make debug-disk``
   Boot QEMU paused with ``-s -S`` for GDB.

``make docs``
   Build HTML docs with Sphinx.

``make clean`` / ``make distclean``
   Remove generated outputs. ``distclean`` also removes Limine.
