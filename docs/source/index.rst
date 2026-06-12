JNU Kernel Internals Manual
============================

JNU is an x86_64 hobby kernel (currently v0.0.4) booted via `Limine
v8 <https://github.com/limine-bootloader/limine>`_. It runs a native
userspace built on ``libjnu`` and can also execute statically-linked
`musl <https://musl.libc.org/>`_ programs when the kernel's
Linux-compatible syscall surface is satisfied.

This manual documents how the kernel is built, how it boots, and how
each major subsystem fits together. It is generated from the source tree
alongside the code and is intended for developers working on or auditing
the kernel — not as an end-user guide.

Quick start
-----------

If you only need to build and run the kernel, start with :doc:`build`.
To link musl programs, see :doc:`musl`.

System overview
---------------

At a high level, JNU follows a conventional monolithic layout:

.. code-block:: text

   Firmware / Limine
        │
        ▼
   kernel_main()          ← arch, MM, drivers, VFS
        │
        ├── initramfs     ← early /init, test binaries (CPIO in memory)
        └── Minix root    ← persistent disk on hda (ATA) or vda (virtio-blk)
        │
        ▼
   /init (ring 3)        ← forks, execs musltest, keyboard echo

**Boot media.** Limine loads ``kernel.elf`` and an ``initramfs.cpio``
module. The kernel parses the CPIO archive for early executables, then
mounts a MINIX v1 filesystem from a block device as ``/``.

**Memory.** Physical pages are managed by a buddy allocator (PMM).
Four-level paging maps virtual addresses; each process has an
``addr_space`` (PML4 + VMA red-black tree). Kernel heap objects use slab
caches behind ``kmalloc``.

**Processes.** A *process* is a thread group: PID (tgid), fd table, and
address space. A *task* is one schedulable thread (kernel stack, saved
registers, unique ``tid``). Since v0.0.4, ``clone(CLONE_VM |
CLONE_THREAD)`` adds threads to a group; ``fork()`` still creates a new
group with one thread. ``futex(2)`` provides the wait/wake primitives
musl ``pthread`` uses (mutex, cond, join). The scheduler is single-CPU
round-robin, preempted by the LAPIC timer.

**Syscalls.** Userspace enters the kernel via ``SYSCALL``/``SYSRET``.
Since v0.0.3, syscall *numbers* match the Linux x86_64 ABI so musl can
link without patches; semantics and error handling remain JNU's. See
:doc:`syscall/interface` and :doc:`syscall/table`.

**Filesystems.** The VFS layer sits above Minix v1 on block devices.
Initramfs files are accessed through a separate fd backing type until
the root disk is mounted.

Suggested reading order
-----------------------

For a first pass through the codebase:

1. :doc:`build` — toolchain, Make targets, QEMU/VMware disk setup
2. :doc:`arch/boot` — Limine handoff and ``kernel_main()`` bring-up order
3. :doc:`mm/pmm` → :doc:`mm/paging` → :doc:`mm/vmm` → :doc:`mm/vma` — memory stack
4. :doc:`proc/process` → :doc:`proc/scheduler` → :doc:`proc/futex` → :doc:`proc/exec` — tasks and ELF load
5. :doc:`syscall/interface` → :doc:`syscall/table` — userspace/kernel boundary
6. :doc:`fs/initramfs` → :doc:`fs/vfs` → :doc:`fs/fd` — file I/O path
7. :doc:`infra/panic` → :doc:`infra/klog` — diagnostics when things go wrong

Repository layout
-----------------

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - Path
     - Contents
   * - ``kernel/``
     - Kernel source: ``arch/x86_64/``, ``mm/``, ``fs/``, ``syscall/``,
       ``drivers/``, ``kernel/`` (main, sched, clone, futex, retire, panic),
       ``user/`` (fd, copy, process)
   * - ``include/jnu/``
     - Kernel-internal headers
   * - ``include/uapi/jnu/``
     - Userspace ABI: syscall numbers, errno, stat, mman
   * - ``user/``
     - Native programs (``init/``, etc.), ``libjnu/`` thin libc, optional
       ``musl/`` and ``musltest/``
   * - ``boot/``
     - ``limine.cfg``, prebuilt Limine binaries
   * - ``scripts/``
     - Build helpers (initramfs, ISO, QEMU, code generation)
   * - ``docs/source/``
     - This manual (reStructuredText, built with Sphinx)

.. toctree::
   :maxdepth: 1
   :caption: Setup and Build

   build
   musl

.. toctree::
   :maxdepth: 1
   :caption: Architecture

   arch/boot
   arch/cpu
   arch/descriptors
   arch/interrupts
   arch/syscall_entry

.. toctree::
   :maxdepth: 1
   :caption: Memory Management

   mm/pmm
   mm/paging
   mm/vmm
   mm/vma
   mm/slab
   mm/kmalloc

.. toctree::
   :maxdepth: 1
   :caption: Process Model

   proc/process
   proc/scheduler
   proc/futex
   proc/exec

.. toctree::
   :maxdepth: 1
   :caption: System Calls

   syscall/interface
   syscall/table

.. toctree::
   :maxdepth: 1
   :caption: Filesystems and I/O

   fs/vfs
   fs/block
   fs/fd
   fs/initramfs

.. toctree::
   :maxdepth: 1
   :caption: Kernel Infrastructure

   infra/klog
   infra/panic
   infra/spinlock
   infra/types
   infra/errno
   infra/compiler
   infra/rbtree
   infra/symbols
   infra/selftest
   infra/cmdline

.. toctree::
   :maxdepth: 1
   :caption: Special Notes

   special_notes

Indices
-------

* :ref:`genindex`
* :ref:`search`
