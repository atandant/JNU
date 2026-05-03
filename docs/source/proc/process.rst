Process Model
=============

The JNU process model separates the *task* (an execution context with a
kernel stack and a scheduler state) from the *process* (a resource container
that holds an address space, a file descriptor table, and a PID).

A process always has exactly one main task in the current implementation.
Future versions may support multiple tasks per process (threads).

Structures
----------

.. code-block:: c

   struct process {
       int                 pid;
       enum process_state  state;       /* PROCESS_ALIVE or PROCESS_ZOMBIE */
       struct task        *main_task;
       struct process     *parent;
       struct process     *first_child;
       struct process     *next_sibling;
       int                 exit_status;
       struct fd_table     fds;
       struct addr_space  *space;
       uint64_t            user_entry;
       uint64_t            user_stack;
       bool                has_user_frame;
       struct syscall_frame user_frame;
   };

``user_entry`` and ``user_stack`` record the entry point and initial stack
pointer for the process's userspace image. ``has_user_frame`` and
``user_frame`` are used by ``sys_fork()`` to preserve the parent's register
state so the child can be forked back into the correct userspace context.

PID Allocation
--------------

PIDs are allocated from a monotonically incrementing counter protected by
an internal lock. ``process_alloc_pid()`` returns the next available PID.
``process_release_pid(pid)`` marks the PID as available for reuse.

.. note::

   The PID allocator in v0.0.2 does not reuse PID slots from exited
   processes during a single boot session. Wraparound is not currently
   handled.

Lifecycle
---------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Function
     - Effect
   * - ``process_create_kernel(task)``
     - Creates the initial kernel process wrapping the boot task.
   * - ``process_fork(frame, &pid_out)``
     - Duplicates the calling process. Clones the address space via
       ``vmm_clone_space()``, duplicates the fd table via
       ``fd_table_clone()``, creates a new task via
       ``sched_create_user_task()``, and schedules it. The child wakes
       directly in userspace with ``rax = 0``.
   * - ``process_execve(path, argv, envp, &entry, &stack)``
     - Replaces the current process image. Frees the old address space,
       loads the ELF binary from the VFS, sets up the initial stack with
       argv/envp, and returns the new entry point and stack pointer.
   * - ``process_exit_current(status)``
     - Marks the process as ``PROCESS_ZOMBIE`` and records ``exit_status``.
       Wakes any task blocked in ``process_wait()``.
   * - ``process_wait(pid, &status)``
     - Blocks until the target process becomes a zombie, then reaps it via
       ``sched_reap_task()``, releases its address space with
       ``vmm_destroy_space()``, and frees the process structure.
   * - ``process_destroy(proc)``
     - Frees all resources held by ``proc``. Called by the reaping path
       after the zombie has been observed.

Fork Semantics
--------------

``process_fork()`` follows POSIX ``fork()`` semantics:

- The parent receives the child PID as the return value of ``sys_fork()``.
- The child receives 0 in ``RAX`` and executes from the same ``RIP`` and
  ``RSP`` as the parent's syscall return site.
- File descriptors are duplicated: ``fd_table_clone()`` bumps the refcount
  on every ``struct file`` so that ``close()`` in either process does not
  affect the other's view of the file until the last reference is dropped.
- The address space is cloned with CoW: all user pages are initially shared
  and write-protected. The first write in either process copies the page.

.. warning::

   ``process_fork`` is called from ring 0 with preemption disabled around
   the fd table clone loop. The ``has_user_frame`` flag must be true when
   ``sys_fork()`` is dispatched; if it is false, ``sys_fork()`` returns
   ``-EINVAL``.
