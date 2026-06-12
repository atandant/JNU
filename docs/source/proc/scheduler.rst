Scheduler
=========

The JNU scheduler implements **preemptive round-robin** scheduling on a
single CPU. Preemption is driven by the LAPIC timer tick
(``sched_tick()`` from vector 48).

Source: ``kernel/kernel/sched.c``, ``include/jnu/kernel/sched.h``.
Context switch: ``kernel/arch/x86_64/context.S``.

Task structure
--------------

.. code-block:: c

   struct task {
       int              tid;           /* unique thread id */
       int              pid;           /* thread-group id (tgid) */
       enum task_state  state;
       struct context   ctx;           /* saved GPRs for context_switch */
       void            *kstack_base;
       void            *kstack_top;
       struct process  *process;
       struct task     *parent;
       int              exit_status;
       unsigned int     wake_pending;
       struct task     *run_next;      /* runqueue link */
       struct task     *all_next;      /* global task list */
       struct task     *task_next;     /* thread-group list (proc->tasks) */
       const char      *name;
       uint32_t         flags;         /* TIF_* pending work */
       void            *clear_child_tid;
       void            *set_child_tid; /* CLONE_CHILD_SETTID (child writes) */
       bool             has_user_frame;
       struct syscall_frame user_frame; /* clone child first-run frame */
       uint64_t         fs_base;       /* TLS via arch_prctl */
       uint64_t         gs_base;
       uint8_t          fpu_state[1024];
   };

Kernel stacks are ``KSTACK_ORDER`` 2 buddy pages (16 KiB). User tasks
enter ring 3 via ``usermode_enter()`` or ``usermode_enter_fork_frame()``
on first run.

Task states
-----------

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - State
     - Description
   * - ``TASK_RUNNABLE``
     - Eligible to run; on the runqueue.
   * - ``TASK_RUNNING``
     - Currently executing on the CPU.
   * - ``TASK_SLEEPING``
     - Blocked (e.g. ``wait4``); not on runqueue until ``sched_wake()``.
   * - ``TASK_ZOMBIE``
     - Last thread of a group exited; struct remains until parent
       ``wait4`` reaps via ``sched_reap_task()``.
   * - ``TASK_DEAD``
     - Detached thread exited (siblings still live). Self-reaped on the
       next context switch; never visible to ``wait4``.

Pending-work flags
------------------

``TIF_NEED_DIE`` is set by ``process_group_exit()`` on sibling tasks.
``signal_pending()`` tests it; ``arch_return_to_user_work()`` consumes
it at return-to-userspace boundaries (syscall G1, IRQ G2) and calls
``process_thread_exit()`` with ``proc->group_exit_code``. Interruptible
sleeps poll the flag and return ``-EINTR`` toward the same gates.

Thread and task limits
----------------------

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Constant
     - Purpose
   * - ``JNU_MAX_THREADS_PER_GROUP`` (256)
     - Maximum live threads per ``struct process``. ``clone`` returns
       ``-EAGAIN`` when exceeded.
   * - ``JNU_MAX_TASKS`` (512)
     - Maximum ``struct task`` allocations system-wide (all creation
       paths). Returns ``-EAGAIN`` when exceeded.

Runqueue and quantum
--------------------

The runqueue is a FIFO singly-linked list via ``task->run_next``.
``sched_tick()`` (every LAPIC timer interrupt):

1. Decrements ``quantum_left`` (``SCHED_QUANTUM_TICKS == 1``).
2. If quantum expired and more than one runnable task exists, rotates the
   current task to the tail and switches to the new head.
3. Calls ``context_switch(&prev->ctx, &next->ctx)``.

A separate ``all_tasks`` list tracks every task for debugging, global
task counting, and reap.

Context switch
--------------

``context_switch()`` in ``context.S`` saves callee-saved registers
(``rbx``, ``rbp``, ``r12``–``r15``) and ``rsp`` in ``prev->ctx``, loads
``next->ctx``, and jumps to where ``next`` last yielded.

On switch to a different address space, the scheduler calls
``vmm_switch_to(next->process->space)`` and updates syscall scratch kernel
stack via ``arch_syscall_set_kernel_stack()``. ``tss_set_rsp0()`` is updated
to ``next->kstack_top`` for double-fault / IST safety.

FPU state is saved/restored separately in ``fpu.c`` when switching tasks.

Deferred reap (``TASK_DEAD``)
-----------------------------

A detached thread cannot free the kernel stack it is still executing on.
``switch_to()`` records the outgoing ``TASK_DEAD`` task in ``reap_zombie``
just before the context switch (with ``sched_lock`` held). The incoming
task frees it in ``sched_finish_switch()``: remove from ``all_tasks``,
free kstack pages, ``kfree(struct task)``.

User task first run
-------------------

**Thread-group leader** (new process or after ``execve``):
``sched_create_user_task()`` → ``user_thread_entry`` →
``usermode_enter(proc->user_entry, proc->user_stack)``.

**Cloned thread:** ``sched_create_thread_task()`` forges
``task->user_frame`` from the parent's syscall frame (``rsp = child_stack``,
``rax = 0`` on entry) and starts at ``thread_user_entry``. Before entering
ring 3, the child writes ``CLONE_CHILD_SETTID`` (if requested). Subsequent
preemption saves/restores kernel context on the task's kernel stack.

Missed-wakeup avoidance
-----------------------

Race between ``sched_wake()`` and ``sched_sleep_current()``:

1. Waker sees target still ``TASK_RUNNING`` → increment ``wake_pending``.
2. Sleeper checks ``wake_pending`` before sleeping → if non-zero, decrement
   and stay runnable.

Safe on single-CPU with IRQ-disabled critical sections. SMP would need
atomics on ``wake_pending``.

API summary
-----------

.. code-block:: c

   void sched_init(void);

Initialize runqueue; create boot and idle tasks.

.. code-block:: c

   struct task *sched_current(void);

Currently running task.

.. code-block:: c

   int sched_create_kernel_thread(const char *name, kernel_thread_fn fn,
                                  void *arg, struct task **out);

Kernel-only task; starts at ``fn(arg)``.

.. code-block:: c

   int sched_create_user_task(const char *name, struct process *proc,
                              struct task **out);

User task bound to ``proc`` as thread-group leader; first run enters ring 3.

.. code-block:: c

   int sched_create_thread_task(struct process *proc,
                                const struct syscall_frame *parent_frame,
                                uint64_t child_stack, uint64_t tls,
                                void *clear_child_tid, void *set_child_tid,
                                struct task **out);

Add a thread to an existing group. Validates ``child_stack`` with
``user_range_ok``. Allocates ``tid`` via ``process_alloc_pid()``.

.. code-block:: c

   void sched_yield(void);

Move current task to runqueue tail and switch.

.. code-block:: c

   void sched_exit_current(int status);

Mark zombie and yield; task not scheduled again.

.. code-block:: c

   void sched_exit_detached(void);

Mark ``TASK_DEAD`` and schedule away forever; next task frees this task.

.. code-block:: c

   void sched_sleep_current(void);

Block until ``sched_wake()`` (or pending wake).

.. code-block:: c

   int sched_sleep_interruptible(void);

Like ``sched_sleep_current`` but returns ``-EINTR`` if ``TIF_NEED_DIE``.

.. code-block:: c

   void sched_wake(struct task *task);

Runnable from sleeping, or bump ``wake_pending``.

.. code-block:: c

   bool signal_pending(void);

True if the current task has a pending ``TIF_*`` flag.

.. code-block:: c

   void sched_reap_task(struct task *task);

Free zombie task kernel stack and struct.

Idle loop
---------

After ``start_init()``, ``kernel_main()`` enters ``kernel_idle_loop()``:
``sti; hlt`` forever. User and kernel runnable tasks preempt the idle
path via timer interrupts.

Related docs
------------

* Timer IRQ: :doc:`/arch/interrupts`
* Process lifecycle and thread groups: :doc:`process`
