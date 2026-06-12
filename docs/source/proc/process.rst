Process Model
=============

The JNU process model separates the *task* (schedulable execution context
with kernel stack and saved registers) from the *process* (resource
container and **thread group**: PID/tgid, fd table, address space).

Since v0.0.4 a process may own multiple tasks (threads) that share the
same ``struct process``. ``fork()`` still creates a new process with a
single thread; ``clone(CLONE_VM | CLONE_THREAD | …)`` adds a thread to
the caller's group. Source: ``kernel/user/process.c``,
``kernel/kernel/clone.c``, ``include/jnu/kernel/process.h``.

Structures
----------

.. code-block:: c

   struct process {
       int                 pid;            /* thread-group id (tgid) */
       enum process_state  state;         /* PROCESS_ALIVE or PROCESS_ZOMBIE */
       struct task        *main_task;     /* group leader; wait4 wake target */
       struct task        *tasks;         /* thread-group list (task_next) */
       int                 live_threads;    /* non-exited tasks in group */
       struct process     *parent;
       struct process     *first_child;
       struct process     *next_sibling;
       int                 exit_status;
       int                 group_exit_code; /* exit_group() status */
       struct fd_table     fds;
       struct addr_space  *space;
       uint64_t            user_entry;
       uint64_t            user_stack;
       bool                has_user_frame;
       struct syscall_frame user_frame;
   };

Each ``struct task`` carries its own ``tid`` (unique across the system)
and ``pid`` (the thread group's tgid, equal to ``proc->pid`` for every
thread in the group). See :doc:`scheduler` for per-task fields
(``task_next``, ``clear_child_tid``, ``set_child_tid``, ``flags``).

``user_entry`` / ``user_stack`` — ELF entry and initial stack for the
**thread-group leader's** first run via ``usermode_enter()``.

``has_user_frame`` / ``user_frame`` — saved syscall return context for
``fork()`` so the child process's leader resumes in userspace at the
parent's syscall site with ``RAX = 0``. Cloned threads use a per-task
``user_frame`` instead (see :doc:`scheduler`).

Thread groups
-------------

All tasks in a group share:

* ``proc->space`` (address space)
* ``proc->fds`` (open-file table)
* ``proc->pid`` (exposed as ``getpid()`` via ``task->pid``)

They are linked on ``proc->tasks`` via ``task->task_next``.
``proc->live_threads`` counts tasks that have not yet exited. When it
reaches zero the group is torn down and the process becomes a zombie for
``wait4()``.

``main_task`` is the group leader — the task ``wait4`` wakes on the
parent and the fallback for reparent notifications. If the leader exits
while siblings remain, ``main_task`` advances to the next linked task.

Process tree
------------

Processes form a tree linked by ``parent``, ``first_child``, and
``next_sibling``. PID 1 is the init process registered via
``process_set_init()`` during ``start_init()``.

When a parent exits before its children, children are reparented to init
(so zombies can still be reaped). A parent blocked in ``wait4`` is woken
when a child **process** (thread group) becomes a zombie.

PID and TID allocation
----------------------

Both process PIDs and per-thread TIDs are drawn from the same monotonic
counter via ``process_alloc_pid()``. This prevents a thread ``tid`` from
colliding with an unrelated process ``pid`` (important once ``tgkill`` and
signals land). Released IDs are not reused within a single boot session.

The thread-group leader's ``tid`` equals ``proc->pid`` at creation.
Additional threads receive a fresh ``tid`` while keeping ``task->pid ==
proc->pid``.

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
   * - ``process_clone_thread(…)``
     - Add a thread to the current group (``clone`` path). Shares space
       and fds; forges per-task userspace frame on ``child_stack``.
   * - ``process_thread_exit(status)``
     - Exit one thread. Non-last threads self-reap as ``TASK_DEAD``;
       the last thread runs full teardown and becomes ``TASK_ZOMBIE``.
   * - ``process_group_exit(status)``
     - ``exit_group``: set ``TIF_NEED_DIE`` on siblings, then exit caller.
   * - ``process_execve(path, argv, envp, &entry, &stack)``
     - Destroy old address space, load ELF from initramfs or VFS, build
       argv/envp stack, return new entry and stack.
   * - ``process_exit_current(status)``
     - Close fds, reparent children, mark zombie, wake waiters.
   * - ``process_wait(pid, &status)``
     - Block until target zombie; reap every task in the group, destroy
       address space, free process struct.
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

Clone (threads) semantics
-------------------------

``sys_clone()`` implements the subset of ``clone(2)`` that musl's
``pthread_create()`` uses. Source: ``kernel/syscall/sys_clone.c``,
``kernel/kernel/clone.c``.

**Required flags:** ``CLONE_VM`` and ``CLONE_THREAD`` (same address space,
same thread group). Any other flag outside the accepted set returns
``-ENOSYS``.

**Accepted helper flags** (see ``include/uapi/jnu/sched.h``):
``CLONE_FS``, ``CLONE_FILES``, ``CLONE_SIGHAND``, ``CLONE_SYSVSEM``,
``CLONE_SETTLS``, ``CLONE_PARENT_SETTID``, ``CLONE_CHILD_CLEARTID``,
``CLONE_CHILD_SETTID``, ``CLONE_DETACHED``.

**x86_64 ABI:** ``clone(flags, child_stack, parent_tid, child_tid, tls)``
→ ``RDI``, ``RSI``, ``RDX``, ``R10``, ``R8``.

* Parent: return value = new thread ``tid``.
* Child: does not return through the syscall; resumes in userspace on
  ``child_stack`` with ``RAX = 0`` via a forged copy of the parent's
  syscall frame (``thread_user_entry`` → ``usermode_enter_fork_frame``).
* ``CLONE_SETTLS``: child's FS base (``arch_prctl`` / musl TLS).
* ``CLONE_PARENT_SETTID``: parent writes child ``tid`` to ``parent_tid``.
* ``CLONE_CHILD_SETTID``: **child** writes its ``tid`` to ``child_tid``
  at first userspace entry (not in the parent), avoiding a tid race.
* ``CLONE_CHILD_CLEARTID``: records ``child_tid`` as ``clear_child_tid``;
  on thread exit the kernel writes ``0`` there (``futex_wake`` is TODO).

**Validation and limits:**

* ``child_stack`` must be non-zero and pass ``user_range_ok(…, 1)`` or
  the syscall returns ``-EFAULT``.
* Per-group cap ``JNU_MAX_THREADS_PER_GROUP`` (256): returns ``-EAGAIN``.
* Global task cap ``JNU_MAX_TASKS`` (512): returns ``-EAGAIN``.

Exit vs exit_group
------------------

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - Syscall
     - Behavior
   * - ``exit`` (60)
     - Exit **only the calling thread**. If siblings remain, the task
       becomes ``TASK_DEAD`` and is self-reaped on the next context
       switch. If it is the last thread, full process teardown runs and
       the task becomes ``TASK_ZOMBIE`` for the parent's ``wait4``.
   * - ``exit_group`` (231)
     - Terminate the **entire thread group**. Sets ``TIF_NEED_DIE`` on
       every sibling and wakes sleepers; each thread retires at its next
       return-to-userspace gate (see :doc:`/arch/syscall_entry`). Records
       ``proc->group_exit_code`` so the last thread out reports the
       group's status regardless of teardown order.

Cooperative retirement uses ``arch_return_to_user_work()`` in
``kernel/kernel/retire.c`` (gates G1 syscall return, G2 IRQ return).
Blocking syscalls such as ``wait4`` use ``sched_sleep_interruptible()``,
which returns ``-EINTR`` when ``TIF_NEED_DIE`` is pending so the thread
can unwind to a gate.

Zombies and waiting
-------------------

On the **last** thread exit, the process becomes ``PROCESS_ZOMBIE`` but
the ``struct process`` persists until the parent calls ``wait4``. The
zombie leader's kernel task is reaped with the rest of the group's
task list in ``process_reap()``.

Detached (non-last) threads are never visible to ``wait4``; their
``struct task`` and kernel stack are freed by the scheduler's deferred
reap path (``reap_zombie`` / ``sched_finish_switch``).

If the parent never waits, the zombie retains PID and minimal memory until
reboot — there is no global reaper beyond init reparenting.

Known gaps (v0.0.4)
-------------------

* ``clear_child_tid`` writes ``0`` on thread exit but does not yet issue
  ``futex_wake`` — ``pthread_join`` may spin until futex support lands.
* No ``tgkill``, ``gettid``, or signal delivery to individual threads.
* Thread limits are fixed constants, not ``RLIMIT_NPROC`` / ``threads-max``.

Related docs
------------

* Scheduling and context switch: :doc:`scheduler`
* ELF load and stack setup: :doc:`exec`
* Syscall entry for fork/clone return frames: :doc:`/arch/syscall_entry`
* Syscall numbers: :doc:`/syscall/table`
