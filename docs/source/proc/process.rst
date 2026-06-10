Process Model
=============

The JNU process model separates the *task* (schedulable execution context
with kernel stack and saved registers) from the *process* (resource
container: PID, fd table, address space).

A process has exactly one main task in v0.0.3. Multiple tasks per process
(threads) are not implemented yet.

Source: ``kernel/user/process.c``, ``include/jnu/kernel/process.h``.

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

``user_entry`` / ``user_stack`` — ELF entry and initial stack for
``usermode_enter()`` on first run or after ``execve``.

``has_user_frame`` / ``user_frame`` — saved syscall return context for
``fork()`` so the child resumes in userspace at the parent's syscall site
with ``RAX = 0``.

Process tree
------------

Processes form a tree linked by ``parent``, ``first_child``, and
``next_sibling``. PID 1 is the init process registered via
``process_set_init()`` during ``start_init()``.

When a parent exits before its children, children are reparented to init
(so zombies can still be reaped). A parent blocked in ``wait4`` is woken
when a child becomes a zombie.

PID allocation
--------------

PIDs come from a monotonically increasing counter (``process_alloc_pid()``).
Released PIDs are not reused within a single boot session in v0.0.3.

Lifecycle
---------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Function
     - Effect
   * - ``process_create_kernel(task)``
     - Wrap the boot kernel task in a process struct.
   * - ``process_fork(frame, &pid_out)``
     - CoW-clone address space, dup fd table refs, create child user
       task, schedule it. Child enters userspace with forged frame.
   * - ``process_execve(path, argv, envp, &entry, &stack)``
     - Destroy old address space, load ELF from initramfs or VFS, build
       argv/envp stack, return new entry and stack.
   * - ``process_exit_current(status)``
     - Mark zombie, record exit status, wake waiters.
   * - ``process_wait(pid, &status)``
     - Block until target zombie; reap task, destroy address space, free
       process struct.
   * - ``process_destroy(proc)``
     - Final resource cleanup after reap.

Boot path: ``start_init()``
---------------------------

Called from ``kernel_main()`` unless ``noinit=1``:

1. Resolve init path: ``cmdline_get("init")`` or default ``/init``.
2. ``process_create()`` — allocate PID, fd table, ``vmm_create_space()``.
3. ``load_boot_exec()`` — try initramfs first, then VFS path.
4. ``process_set_init()`` — mark as PID 1 for reparenting.
5. ``sched_create_user_task("init", proc, …)`` — enqueue user task.
6. Boot CPU task remains in ring 0; first schedule runs
   ``user_thread_entry`` → ``usermode_enter(entry, stack)``.

Init program behavior (``user/init/main.c``): print banner, ``fork()``,
child ``execve("/bin/musltest", …)``, parent ``wait4()``, then keyboard
echo on ``/dev/kbd``.

Fork semantics
--------------

POSIX-like ``fork()`` behavior:

* Parent: ``sys_fork`` return value = child PID.
* Child: ``RAX = 0``, same ``RIP``/``RSP`` as parent's syscall return.
* File descriptors: ``fd_table_clone()`` shares ``struct file`` refs;
  ``close()`` in one process does not unmap the file for the other until
  the last refcount drops.
* Memory: ``vmm_clone_space()`` — shared read-only user pages until CoW
  write fault.

.. warning::

   ``sys_fork()`` requires ``has_user_frame`` true (caller must be a user
   task that entered via syscall). Returns ``-EINVAL`` otherwise.

   The fd clone loop runs with preemption disabled on single-CPU builds.

Zombies and waiting
-------------------

On ``exit``, the process becomes ``PROCESS_ZOMBIE`` but the ``struct
process`` persists until the parent calls ``wait4``. The zombie's kernel
task is reaped separately via ``sched_reap_task()``.

If the parent never waits, the zombie retains PID and minimal memory until
reboot — there is no global reaper beyond init reparenting.

Related docs
------------

* Scheduling and context switch: :doc:`scheduler`
* ELF load and stack setup: :doc:`exec`
* Syscall entry for fork return frame: :doc:`/arch/syscall_entry`
