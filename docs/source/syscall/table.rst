System Call Table
=================

Since v0.0.3, JNU uses **Linux x86_64 syscall numbers** for every
implemented syscall. The integers match what musl and glibc expect on
x86_64; register layout, return conventions, and semantics remain JNU's.
Numbers are defined in ``include/uapi/jnu/syscall_nr.h`` and dispatched
from ``kernel/syscall/dispatch.c``.

Any number not listed below — or any slot in the sparse table that is
``NULL`` — returns ``-ENOSYS``. JNU-private syscalls (none yet) are
reserved in the range ``[1024, 1535]``.

Dispatch overview
-----------------

``syscall_dispatch()`` bounds-checks ``args->nr`` against ``JNU_SYS_MAX``
(318), indexes a static handler table, and returns the handler's
``int64_t`` result unchanged. Wrappers in ``dispatch.c`` adapt each
handler's real prototype to a uniform ``(const struct syscall_args *)``
signature.

Implemented syscalls
--------------------

.. list-table::
   :header-rows: 1
   :widths: 5 22 28 45

   * - Nr
     - Name
     - Handler
     - Summary
   * - 0
     - ``read``
     - ``sys_read``
     - Read from an open fd into a user buffer. Advances file offset.
   * - 1
     - ``write``
     - ``sys_write``
     - Write to an open fd from a user buffer.
   * - 2
     - ``open``
     - ``sys_open``
     - Open a path (initramfs, VFS, or chardev). ``path`` copied via
       ``syscall_copy_path()``.
   * - 3
     - ``close``
     - ``sys_close``
     - Close fd; drop open-file refcount.
   * - 5
     - ``fstat``
     - ``sys_fstat``
     - Fill ``struct jnu_stat`` for an open file.
   * - 8
     - ``lseek``
     - ``sys_lseek``
     - Reposition file offset (``SEEK_SET``/``CUR``/``END``).
   * - 9
     - ``mmap``
     - ``sys_mmap``
     - Map anonymous private pages (lazy fault-in). See :doc:`/mm/vmm`.
   * - 10
     - ``mprotect``
     - ``sys_mprotect``
     - Change VMA permissions and PTE flags.
   * - 11
     - ``munmap``
     - ``sys_munmap``
     - Unmap a VMA range; tear down PTEs.
   * - 13
     - ``rt_sigaction``
     - ``sys_rt_sigaction``
     - **Stub:** returns 0. No signal delivery in v0.0.3; satisfies musl
       static startup.
   * - 14
     - ``rt_sigprocmask``
     - ``sys_rt_sigprocmask``
     - **Stub:** returns 0. Same rationale as ``rt_sigaction``.
   * - 16
     - ``ioctl``
     - ``sys_ioctl``
     - **Stub:** returns ``-ENOTTY``. musl probes terminal size and
       falls back gracefully.
   * - 20
     - ``writev``
     - ``sys_writev``
     - Scatter-write to an fd (used by musl stdio).
   * - 24
     - ``sched_yield``
     - ``sys_yield``
     - Voluntarily yield the CPU; always returns 0.
   * - 35
     - ``nanosleep``
     - ``sys_nanosleep``
     - Sleep for a relative interval (scheduler-based).
   * - 39
     - ``getpid``
     - ``sys_getpid``
     - Return calling thread group's PID (``task->pid`` / tgid).
   * - 56
     - ``clone``
     - ``sys_clone``
     - Create a thread in the current group (``CLONE_VM | CLONE_THREAD``
       plus musl TLS/tid flags). Parent returns new ``tid``; child resumes
       on ``child_stack`` with ``RAX = 0``. See :doc:`/proc/process`.
   * - 57
     - ``fork``
     - ``sys_fork``
     - Duplicate process (CoW address space, shared fd table refs).
       Child returns 0 in ``RAX``.
   * - 59
     - ``execve``
     - ``sys_execve``
     - Replace process image from ELF on initramfs or VFS.
   * - 60
     - ``exit``
     - ``sys_exit``
     - Terminate **calling thread** only. Last thread performs full
       process teardown; never returns.
   * - 61
     - ``wait4``
     - ``sys_waitpid``
     - Block until child exits. ``options`` and ``rusage`` ignored;
       forwarded to existing wait implementation.
   * - 74
     - ``fsync``
     - ``sys_fsync``
     - Flush file metadata to disk (Minix).
   * - 77
     - ``ftruncate``
     - ``sys_ftruncate``
     - Truncate open file to ``length``.
   * - 82
     - ``rename``
     - ``sys_rename``
     - Rename path on root Minix filesystem.
   * - 83
     - ``mkdir``
     - ``sys_mkdir``
     - Create directory on root Minix filesystem.
   * - 84
     - ``rmdir``
     - ``sys_rmdir``
     - Remove empty directory.
   * - 85
     - ``creat``
     - ``sys_creat``
     - Create/truncate file (``open`` with create flags).
   * - 87
     - ``unlink``
     - ``sys_unlink``
     - Remove file from root Minix filesystem.
   * - 158
     - ``arch_prctl``
     - ``sys_arch_prctl``
     - TLS setup: ``ARCH_SET_FS``, ``ARCH_GET_FS`` for musl thread pointer.
   * - 202
     - ``futex``
     - ``sys_futex``
     - ``FUTEX_WAIT`` / ``FUTEX_WAKE`` / ``FUTEX_REQUEUE`` for musl
       pthread primitives. See :doc:`/proc/futex`.
   * - 218
     - ``set_tid_address``
     - ``sys_set_tid_address``
     - Store ``clear_child_tid`` on the calling task; return caller's
       ``tid``. On thread exit the kernel writes ``0`` and issues
       ``futex_wake`` so ``pthread_join`` can complete.
   * - 228
     - ``clock_gettime``
     - ``sys_clock_gettime``
     - Monotonic and realtime clocks (TSC-based).
   * - 231
     - ``exit_group``
     - ``sys_exit_group``
     - Terminate entire thread group (``TIF_NEED_DIE`` on siblings);
       never returns. musl ``_Exit`` / ``abort`` route here.
   * - 318
     - ``getrandom``
     - ``sys_getrandom``
     - Non-blocking PRNG bytes for userspace.

Historical note
---------------

JNU v0.0.2 used a compact native numbering scheme (0–12) with names like
``JNU_SYS_spawn``. That ABI is **retired**. Native ``libjnu`` programs
now issue Linux numbers via ``user/libjnu/syscall.S``. The UAPI header
``include/uapi/jnu/syscall_nr.h`` is the single source of truth; it is
copied into ``user/libjnu/include/jnu_syscall.h`` at build time.

File descriptor types
---------------------

Open files are backed by one of three types (see :doc:`/fs/fd`):

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - ``jnu_file_type``
     - Description
   * - ``JNU_FILE_INITRAMFS``
     - In-memory CPIO entry. Read-only; used for early ``/init`` and
       test binaries.
   * - ``JNU_FILE_VFS``
     - Minix v1 inode on the mounted root disk. Read/write via VFS ops.
   * - ``JNU_FILE_CHARDEV``
     - Character device (``/dev/kbd``, ``/dev/serial``). Keyboard read
       returns scancode-derived bytes.

``struct jnu_stat``
-------------------

Filled by ``fstat`` into userspace:

.. code-block:: c

   struct jnu_stat {
       uint64_t ino;   /* Inode number or synthetic id */
       uint64_t size;  /* File size in bytes */
       uint32_t mode;  /* Permission bits */
       uint32_t type;  /* jnu_file_type tag */
   };

Common unimplemented Linux syscalls
-----------------------------------

musl's static link path may probe additional syscalls. The following
commonly appear in strace-style debugging but return ``-ENOSYS`` today:

* ``brk``, ``mremap``, ``madvise``, ``prlimit64``
* ``openat``, ``newfstatat``, ``readlink``, ``getcwd``
* Full signal delivery (``kill``, ``tgkill``, ``rt_sigreturn``)
* ``gettid`` (musl can use ``clone`` return value instead)
* ``pipe``, ``socket``, ``poll``, ``epoll_*``

When adding musl support for a new program, compare its startup syscall
sequence against this table and implement or stub missing entries as
needed. See :doc:`/musl` for the build and test workflow.
