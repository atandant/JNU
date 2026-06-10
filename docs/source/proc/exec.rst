ELF Execution
=============

The execution subsystem is responsible for loading ELF64 binaries into
user address spaces. It is split into three layers: the abstract image
interface, the ELF64 loader, and the exec adapter layer that binds specific
file sources (initramfs, VFS) to the loader.

Exec Image Abstraction
----------------------

.. code-block:: c

   struct exec_image {
       ssize_t (*read_at)(void *ctx, uint64_t off, void *buf, size_t len);
       uint64_t size;
       void    *ctx;
   };

``exec_image`` decouples the ELF loader from the backing storage. The
``read_at`` callback reads ``len`` bytes at offset ``off`` into ``buf``.
The initramfs adapter passes a pointer into the mapped CPIO archive;
the VFS adapter calls ``vfs_read()``.

Load Information
----------------

.. code-block:: c

   struct exec_load_info {
       uint64_t entry;  /* ELF entry point virtual address */
       uint64_t low;    /* Lowest virtual address of any PT_LOAD segment */
       uint64_t high;   /* Highest virtual address (exclusive) of any PT_LOAD */
   };

ELF64 Loader
------------

.. code-block:: c

   int elf64_validate_image(const struct exec_image *image,
                            struct exec_load_info *info);

Reads the ELF header and program headers to verify the magic, class
(``ELFCLASS64``), machine (``EM_X86_64``), and type (``ET_EXEC`` or
``ET_DYN``). Populates ``info->entry``, ``info->low``, and ``info->high``
without performing any mapping. Returns 0 or a negative errno.

.. code-block:: c

   int elf64_load_image(struct addr_space *space,
                        const struct exec_image *image,
                        struct exec_load_info *info);

Iterates all ``PT_LOAD`` program headers and maps each into ``space``:

1. The physical backing pages are allocated via ``pmm_alloc_user_page()``.
2. The segment data is read from the image into the pages.
3. The pages are mapped into ``space`` at the virtual address specified in
   the program header, with permissions derived from the ``p_flags`` field
   (``PF_R``, ``PF_W``, ``PF_X``).
4. If ``p_filesz < p_memsz`` (BSS region), the trailing pages are zeroed.

.. code-block:: c

   int elf64_setup_initial_stack(struct addr_space *space,
                                 const struct exec_strings *strings,
                                 uint64_t *stack_out);

Allocates a user stack (8 pages by default) below ``USER_TOP`` and writes
the ``argv`` and ``envp`` string tables plus the auxv vector at the top.
Returns the initial stack pointer in ``*stack_out``.

Exec Strings
------------

.. code-block:: c

   struct exec_strings {
       char   *path;
       char  **argv;
       char  **envp;
       void   *backing;  /* single allocation holding all string data */
       size_t  argc;
       size_t  envc;
       size_t  total;    /* total bytes in backing */
   };

``exec_strings_capture(user_path, user_argv, user_envp, &out)`` copies
path, argv, and envp from user space into a single kernel allocation using
``copy_from_user()``. All userspace pointers are validated before
dereferencing.

``exec_strings_release(&strings)`` frees the backing allocation.

High-Level Interface
--------------------

.. code-block:: c

   int process_execve(const char *user_path, char *const *user_argv,
                      char *const *user_envp,
                      uint64_t *entry_out, uint64_t *stack_out);

Called by ``sys_execve()``. Captures the exec strings, destroys the current
address space, creates a new one, loads the ELF image from the VFS, sets
up the initial stack, and returns the new entry point and stack pointer.
The calling task is expected to return to userspace via ``usermode_enter()``
(for boot-time init) or by restoring a forged ``syscall_frame`` (for
``execve`` within a running process).

Boot vs syscall exec paths
--------------------------

Two paths reach a loaded ELF in user space:

* **Boot** — ``start_init()`` loads ``/init``, then
  ``usermode_enter(entry, stack)`` via ``iretq`` (first ring-3 entry).
* **Syscall** — ``sys_execve()`` replaces the address space and forges a
  ``syscall_frame`` so ``SYSRET`` resumes at the new entry with the new
  stack (same task, same PID).

Both use ``process_execve()`` → ``elf64_load_image()`` and
``elf64_setup_initial_stack()``.

ELF types and validation
------------------------

``elf64_validate_image()`` accepts ``ET_EXEC`` and ``ET_DYN`` (PIE) for
x86_64. Program headers must be ``PT_LOAD`` only for mapping; invalid
magic, class, or machine returns ``-ENOEXEC``.

Stack layout places ``argc``, ``argv`` pointers, ``envp`` pointers, and
auxiliary vector entries (``AT_NULL`` terminator minimum) on the initial
user stack. musl expects ``AT_PAGESZ`` and related auxv entries where
implemented in ``elf64_setup_initial_stack()``.

Load order for paths
--------------------

``load_boot_exec()`` (init) and ``sys_open`` resolution try **initramfs
first**, then **VFS** root. Early boot binaries live only in the CPIO;
after Minix mount, ``/bin/*`` on disk is used.

Internal adapters
-----------------

``load_initramfs_exec()``, ``load_vfs_exec()``, and validate-only variants
live in ``kernel/kernel/initfs_exec.c`` and ``kernel/kernel/vfs_exec.c``.
They wrap ``struct exec_image`` with initramfs or VFS ``read_at``
callbacks — not exported as public API.

Related docs
------------

* VMA permissions per segment: :doc:`/mm/vmm`
* Initramfs lookup: :doc:`/fs/initramfs`
* First ring-3 entry: :doc:`/arch/syscall_entry`
