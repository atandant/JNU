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
       int              tid;
       int              pid;
       enum task_state  state;        /* RUNNABLE, RUNNING, SLEEPING, ZOMBIE */
       struct context   ctx;          /* Saved GPRs for context_switch */
       void            *kstack_base;
       void            *kstack_top;
       struct process  *process;
       struct task     *parent;
       int              exit_status;
       unsigned int     wake_pending; /* Missed-wakeup counter */
       struct task     *run_next;     /* Runqueue link */
       struct task     *all_next;     /* All-tasks list */
       const char      *name;
   };

Kernel stacks are ``KSTACK_ORDER`` 2 buddy pages (16 KiB). User tasks
additionally have userspace entry/stack in ``process`` and enter ring 3
via ``usermode_enter()`` on first run.

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
     - Exited; struct remains until ``sched_reap_task()``.

Runqueue and quantum
--------------------

The runqueue is a FIFO singly-linked list via ``task->run_next``.
``sched_tick()`` (every LAPIC timer interrupt):

1. Decrements ``quantum_left`` (``SCHED_QUANTUM_TICKS == 1`` in v0.0.3).
2. If quantum expired and more than one runnable task exists, rotates the
   current task to the tail and switches to the new head.
3. Calls ``context_switch(&prev->ctx, &next->ctx)``.

A separate ``all_tasks`` list tracks every task for debugging and reap.

Context switch
--------------

``context_switch()`` in ``context.S`` saves callee-saved registers
(``rbx``, ``rbp``, ``r12``–``r15``) and ``rsp`` in ``prev->ctx``, loads
``next->ctx``, and jumps to where ``next`` last yielded.

On switch to a different address space, the scheduler calls
``vmm_switch_to(next->process->space)`` and updates syscall scratch kernel
stack via ``arch_syscall_set_kernel_stack()``.

FPU state is saved/restored separately in ``fpu.c`` when switching tasks.

User task first run
-------------------

``sched_create_user_task()`` sets the task entry to ``user_thread_entry``,
which calls ``usermode_enter(proc->user_entry, proc->user_stack)`` and
does not return. Subsequent preemption saves/restores kernel context on the
task's kernel stack; userspace state lives in the process syscall frame or
hardware on syscall boundary.

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

User task bound to ``proc``; first run enters ring 3.

.. code-block:: c

   void sched_yield(void);

Move current task to runqueue tail and switch.

.. code-block:: c

   void sched_exit_current(int status);

Mark zombie and yield; task not scheduled again.

.. code-block:: c

   void sched_sleep_current(void);

Block until ``sched_wake()`` (or pending wake).

.. code-block:: c

   void sched_wake(struct task *task);

Runnable from sleeping, or bump ``wake_pending``.

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
* Process lifecycle: :doc:`process`
