Compiling musl for JNU
======================

JNU can run statically-linked musl userspace programs built with stock
``clang`` and ``ld.lld``. The kernel implements the Linux-compatible
syscall surface that musl's static startup path expects (TLS via
``arch_prctl``, ``writev``, ``getrandom``, and a handful of minimal
stubs). **No JNU-specific patches to musl are required.**

The musl source tree is not vendored in the repository (``user/musl/`` is
gitignored). You must clone, configure, and install musl locally before
``make musltest`` or ``make iso-musl`` will succeed.

Prerequisites
-------------

Use the same host toolchain as the normal JNU build (see :doc:`build`):

* ``clang``
* ``ld.lld``
* ``make``
* ``git``

The build host should be **x86_64 Linux or WSL2**. musl's configure
script builds for the host architecture; JNU only runs x86_64 guests.

To exercise the full ``musltest`` program (MINIX read/write syscalls),
also install ``mkfs.minix`` (``util-linux`` on Debian/Ubuntu) and
``qemu-system-x86_64`` for boot testing.

Obtain musl source
------------------

Clone upstream musl into ``user/musl/`` from the repository root:

.. code-block:: sh

   git clone https://git.musl-libc.org/git/musl \
     --branch v1.2.5 --depth 1 user/musl

Pin the tag above for reproducibility. A newer musl release may work, but
if its static startup path issues syscalls JNU does not implement yet,
``make musltest`` or runtime boot will fail until the kernel catches up.

Configure, build, and install
-----------------------------

From the repository root:

.. code-block:: sh

   cd user/musl
   CC=clang ./configure \
     --prefix="$(pwd)/install" \
     --disable-shared
   make -j"$(nproc)"
   make install
   cd ../..

Important details:

* ``--prefix="$(pwd)/install"`` installs headers and static libraries
  under ``user/musl/install/``. This path is hardcoded in
  ``scripts/build-musl-user.sh`` and must contain at least
  ``lib/libc.a``, ``lib/crt1.o``, ``lib/crti.o``, ``lib/crtn.o``, and
  ``include/``.
* ``--disable-shared`` skips the dynamic linker. JNU only supports fully
  static musl binaries and this speeds up the build.
* ``CC=clang`` matches JNU's default compiler. No ``musl-gcc`` wrapper is
  needed because JNU's link step invokes the CRT objects and ``libc.a``
  directly.
* **Do not** set ``--prefix=/usr`` or ``--prefix=/usr/local``. musl's
  own INSTALL guide warns that installing into system paths can break the
  host.

Verify the install:

.. code-block:: sh

   test -f user/musl/install/lib/libc.a && echo OK

Build musl-linked JNU programs
------------------------------

Once musl is installed:

.. code-block:: sh

   make musltest          # -> build/user/bin/musltest
   make iso-musl          # -> build/kernel-musl.iso

There is no ``make run-musl`` target today. To boot the musl ISO in
QEMU with a writable MINIX root (required for ``musltest`` file I/O):

.. code-block:: sh

   make ata-disk
   bash scripts/run-qemu.sh --iso build/kernel-musl.iso --disk build/disk.img

Expected serial output includes ``JNU init: hello from ring 3``, init
forking ``musltest``, and (with a populated MINIX disk)
``musltest: minix rw syscalls OK``.

Building your own musl programs
-------------------------------

Follow the same pattern as ``scripts/build-musl-user.sh``:

**Compile** (example):

.. code-block:: sh

   clang --target=x86_64-unknown-linux-musl \
     -std=gnu17 -ffreestanding -fno-pic -fno-pie -mno-red-zone \
     -nostdinc -isystem user/musl/install/include \
     -O2 -g3 -c myprog.c -o myprog.o

**Link** (fully static):

.. code-block:: sh

   ld.lld -static -nostdlib \
     user/musl/install/lib/crt1.o \
     user/musl/install/lib/crti.o \
     myprog.o \
     user/musl/install/lib/libc.a \
     user/musl/install/lib/crtn.o \
     -o myprog

Unlike native JNU userspace (``scripts/build-user.sh``), **do not** pass
``-mgeneral-regs-only`` or disable SSE. musl uses SSE internally.

Limitations
-----------

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Topic
     - On JNU today
   * - Dynamic linking
     - Not supported; static linking only.
   * - ``ioctl`` / TTY
     - Stub returns ``-ENOTTY``; musl stdio falls back (no cooked TTY).
   * - ``futex`` / pthread
     - ``FUTEX_WAIT`` / ``WAKE`` / ``REQUEUE`` implemented; other futex
       ops return ``-ENOSYS``. See :doc:`/proc/futex`.
   * - ``getrandom``
     - Implemented but documented as non-cryptographic in the kernel.
   * - ``musltest`` file tests
     - Require a writable MINIX root (``make ata-disk`` and a disk
       attached in QEMU), not initramfs-only boot.
   * - Demo program
     - ``user/musltest`` is the current demo; the spec's
       ``user/demo/hello`` tree is not shipped yet.
   * - CI
     - No automated musl build in CI yet.

Troubleshooting
---------------

**``user/musl/install/lib/libc.a not found``**

musl has not been built or was installed to the wrong prefix. Re-run the
configure/install steps above and confirm ``user/musl/install/lib/libc.a``
exists.

**``make musltest`` compiles but musltest file checks fail at runtime**

``musltest`` creates and renames files on the root filesystem. Boot with
``build/disk.img`` attached (see the QEMU command above). Initramfs-only
boot does not provide writable MINIX storage for those tests.

**Link errors mentioning glibc or wrong CRT**

Ensure compile uses ``--target=x86_64-unknown-linux-musl`` and
``-isystem user/musl/install/include``, and link with musl's ``crt*.o``
and ``libc.a`` — not the host glibc.

**musl configure fails on non-x86_64 hosts**

musl must be built for x86_64 to match JNU's guest architecture. Use an
x86_64 Linux machine or WSL2, or set up a cross toolchain separately
(not covered here).
