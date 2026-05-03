Error Codes
===========

The JNU kernel uses the Linux convention for error returns: functions return
0 on success or the negation of an error code on failure (e.g.,
``-ENOMEM``). All error code symbols are defined in
``include/jnu/errno.h``.

Error Code Table
----------------

.. list-table::
   :header-rows: 1
   :widths: 10 20 70

   * - Value
     - Symbol
     - Description
   * - 1
     - ``EPERM``
     - Operation not permitted. Returned when the calling process lacks
       the necessary privilege for the requested operation.
   * - 2
     - ``ENOENT``
     - No such file or directory. Returned by path resolution and
       ``initramfs_lookup()`` when a name is not found.
   * - 5
     - ``EIO``
     - Input/output error. Returned by block device drivers when a
       hardware-level read or write fails.
   * - 6
     - ``ENXIO``
     - No such device or address. Returned when a device lookup fails or
       an address is not mapped to any device.
   * - 7
     - ``E2BIG``
     - Argument list too long. Returned by ``exec_strings_capture()`` when
       the total size of argv/envp exceeds internal limits.
   * - 8
     - ``ENOEXEC``
     - Executable format error. Returned by ``elf64_validate_image()``
       when the ELF header is malformed or the machine type is wrong.
   * - 10
     - ``ECHILD``
     - No child processes. Returned by ``sys_waitpid()`` when the target
       PID is not a child of the calling process.
   * - 11
     - ``EAGAIN``
     - Try again. Returned when a resource is temporarily unavailable.
   * - 12
     - ``ENOMEM``
     - Out of memory. Returned when the PMM, slab, or ``kmalloc`` cannot
       satisfy an allocation request.
   * - 13
     - ``EACCES``
     - Permission denied. Returned when a file mode check fails.
   * - 14
     - ``EFAULT``
     - Bad address. Returned by ``copy_from_user()``,
       ``copy_to_user()``, and related helpers when a userspace pointer
       fails validation.
   * - 16
     - ``EBUSY``
     - Resource busy. Returned when attempting to mount a filesystem on
       an already-mounted device.
   * - 17
     - ``EEXIST``
     - Already exists. Returned by ``vma_insert()`` and
       ``block_register()`` when a conflicting entry is present.
   * - 19
     - ``ENODEV``
     - No such device. Returned by ``block_lookup()`` when no device
       with the requested name is registered.
   * - 20
     - ``ENOTDIR``
     - Not a directory. Returned when a path component used as a
       directory is a regular file.
   * - 21
     - ``EISDIR``
     - Is a directory. Returned when a directory is passed where a
       regular file is required.
   * - 22
     - ``EINVAL``
     - Invalid argument. The most general error code; returned when an
       argument is out of range, malformed, or logically inconsistent.
   * - 24
     - ``EMFILE``
     - Too many open files. Returned by ``fd_alloc()`` when all 32 fd
       slots in the process fd table are occupied.
   * - 28
     - ``ENOSPC``
     - No space left on device. Reserved; not yet returned by any
       subsystem in v0.0.2.2.
   * - 29
     - ``ESPIPE``
     - Illegal seek. Returned by ``sys_lseek()`` when called on a
       character device, which has no seekable position.
   * - 34
     - ``ERANGE``
     - Out of range. Returned when a numeric value exceeds the valid
       range for its context.
   * - 36
     - ``ENAMETOOLONG``
     - File name too long. Returned by ``copy_string_from_user()`` and
       ``syscall_copy_path()`` when a string exceeds ``JNU_PATH_MAX``
       (256) bytes without a NUL terminator.
   * - 38
     - ``ENOSYS``
     - Function not implemented. Returned by retired or unimplemented
       syscalls (e.g., ``JNU_SYS_spawn``).
   * - 95
     - ``ENOTSUP``
     - Not supported. Returned by ``block_ops::write`` in v0.0.2.2, as
       write support is not yet implemented.

Convention
----------

All kernel functions that can fail return either a negative errno (on
failure) or a non-negative value (on success). The caller is responsible
for interpreting the specific non-negative success value; for many
functions it is simply 0.

Syscall handlers translate kernel errors directly to userspace: the
negated errno is placed in RAX before ``SYSRET``. Userspace libraries
are expected to check ``rax > (uint64_t)-4096`` and negate the value to
recover the errno.
