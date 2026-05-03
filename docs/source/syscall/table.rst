System Call Table
=================

The following table lists all implemented system calls in JNU v0.0.2.2.
Number 9 (``JNU_SYS_spawn``) is retired and returns ``-ENOSYS``.

.. list-table::
   :header-rows: 1
   :widths: 5 20 20 55

   * - Nr
     - Symbol
     - Signature
     - Description
   * - 0
     - ``JNU_SYS_exit``
     - ``sys_exit(status)``
     - Terminates the calling process with ``exit_status = status``.
       Never returns. Calls ``process_exit_current()`` then
       ``sched_exit_current()``.
   * - 1
     - ``JNU_SYS_write``
     - ``sys_write(fd, buf, len)``
     - Writes ``len`` bytes from the user buffer ``buf`` to the open file
       ``fd``. Validates ``buf`` with ``user_range_mapped()``. Returns the
       number of bytes written or a negative errno.
   * - 2
     - ``JNU_SYS_read``
     - ``sys_read(fd, buf, len)``
     - Reads up to ``len`` bytes from ``fd`` into ``buf``. Returns the
       number of bytes read, 0 on EOF, or a negative errno. Advances the
       file offset.
   * - 3
     - ``JNU_SYS_open``
     - ``sys_open(path, flags)``
     - Opens the file at ``path`` and returns a file descriptor. ``path``
       is copied from user space via ``syscall_copy_path()``. Returns
       a non-negative fd on success or a negative errno.
   * - 4
     - ``JNU_SYS_close``
     - ``sys_close(fd)``
     - Closes file descriptor ``fd``, dropping the open-file reference.
       Returns 0 or a negative errno.
   * - 5
     - ``JNU_SYS_lseek``
     - ``sys_lseek(fd, off, whence)``
     - Repositions the file offset. ``whence`` accepts ``SEEK_SET`` (0),
       ``SEEK_CUR`` (1), and ``SEEK_END`` (2). Returns the new absolute
       offset or a negative errno. Rejects negative ``fd`` with
       ``-EBADF``.
   * - 6
     - ``JNU_SYS_getpid``
     - ``sys_getpid()``
     - Returns the PID of the calling process.
   * - 7
     - ``JNU_SYS_yield``
     - ``sys_yield()``
     - Voluntarily yields the CPU. Always returns 0.
   * - 8
     - ``JNU_SYS_fstat``
     - ``sys_fstat(fd, statbuf)``
     - Fills a ``struct jnu_stat`` at ``statbuf`` with inode number, size,
       mode, and file type for the open file ``fd``. Returns 0 or a
       negative errno.
   * - 9
     - ``JNU_SYS_spawn``
     - *retired*
     - Formerly used for process creation before ``fork``/``execve`` were
       implemented. Always returns ``-ENOSYS``.
   * - 10
     - ``JNU_SYS_waitpid``
     - ``sys_waitpid(pid, statusp)``
     - Blocks until the process with the given ``pid`` exits. Writes the
       exit status to ``*statusp`` if ``statusp`` is non-NULL. Rejects
       negative ``pid`` with ``-EINVAL``. Returns ``pid`` on success or
       a negative errno.
   * - 11
     - ``JNU_SYS_fork``
     - ``sys_fork()``
     - Duplicates the calling process. Returns the child PID to the
       parent, and 0 to the child. Requires ``has_user_frame`` to be set
       in the current process. Returns a negative errno on failure.
   * - 12
     - ``JNU_SYS_execve``
     - ``sys_execve(path, argv, envp)``
     - Replaces the current process image with the ELF binary at ``path``.
       ``argv`` and ``envp`` are null-terminated arrays of user string
       pointers, copied via ``exec_strings_capture()``. Does not return
       on success.

File Descriptor Types
---------------------

The kernel recognizes three backing types for open file descriptions:

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - ``jnu_file_type``
     - Description
   * - ``JNU_FILE_INITRAMFS``
     - File backed by the in-memory CPIO initramfs archive. Read-only.
       Seeking is supported.
   * - ``JNU_FILE_VFS``
     - File backed by the VFS layer (currently Minix on ATA). Read-only
       in v0.0.2.
   * - ``JNU_FILE_CHARDEV``
     - File backed by a character device (e.g., the keyboard chardev).
       Read returns typed characters; write is not generally supported.

``struct jnu_stat``
-------------------

The structure filled by ``sys_fstat()``:

.. code-block:: c

   struct jnu_stat {
       uint64_t ino;   /* Inode number */
       uint64_t size;  /* File size in bytes */
       uint32_t mode;  /* Permission bits */
       uint32_t type;  /* jnu_file_type tag */
   };
