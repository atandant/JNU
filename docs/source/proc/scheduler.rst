Scheduler
=========

The JNU scheduler implements preemptive round-robin scheduling. Preemption
is driven by the LAPIC timer tick at vector ``VEC_LAPIC_TIMER`` (48).

Task Structure
--------------

.. code-block:: c

   struct task {
       int              tid;
       int              pid;
       enum task_state  state;        /* RUNNABLE, RUNNING, SLEEPING, ZOMBIE */
       struct context   ctx;          /* Saved register file for context switch */
       void            *kstack_base;
       void            *kstack_top;
       struct process  *process;
       struct task     *parent;
       int              exit_status;
       unsigned int     wake_pending; /* Missed-wakeup counter */
       struct task     *run_next;     /* Link in the runqueue */
       struct task     *all_next;     /* Link in the all-tasks list */
       const char      *name;
   };

Task States
-----------

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - State
     - Description
   * - ``TASK_RUNNABLE``
     - Task is eligible to run and is on the runqueue.
   * - ``TASK_RUNNING``
     - Task is currently executing on the CPU.
   * - ``TASK_SLEEPING``
     - Task is blocked, waiting for a wakeup event. Not on the runqueue.
   * - ``TASK_ZOMBIE``
     - Task has exited. Kernel stack and structure remain allocated until
       reaped by ``sched_reap_task()``.

Runqueue
--------

The runqueue is a singly-linked list chained through ``task->run_next``.
``sched_tick()`` removes the head (the currently running task), moves it
to the tail if it is still ``TASK_RUNNABLE``, and loads the new head. The
``all_tasks`` list is a separate singly-linked chain through
``task->all_next`` that covers all tasks regardless of state.

Missed-Wakeup Avoidance
------------------------

The ``wake_pending`` counter closes the race between ``sched_wake()`` and
``sched_sleep_current()``:

1. A waker calls ``sched_wake(target)`` while ``target->state == TASK_RUNNING``
   (the target has not yet called ``sched_sleep_current()``).
2. ``sched_wake()`` increments ``wake_pending`` instead of trying to add the
   task to the runqueue.
3. When the target calls ``sched_sleep_current()``, it checks
   ``wake_pending``. If non-zero, it decrements the counter and returns
   immediately without sleeping.

This scheme is safe in the single-CPU build because interrupts are disabled
around the critical sections. SMP requires converting ``wake_pending`` to an
atomic.

API
---

.. code-block:: c

   void sched_init(void);

Initializes the runqueue and creates the initial kernel task for the boot
CPU.

.. code-block:: c

   struct task *sched_current(void);

Returns the currently executing task. Reads the per-CPU block; callable
from any kernel context.

.. code-block:: c

   int sched_create_kernel_thread(const char *name, kernel_thread_fn fn,
                                  void *arg, struct task **out);

Allocates a kernel-mode task and a 4 KiB kernel stack. Sets up the context
so the task begins execution at ``fn(arg)``. Places the task on the
runqueue in ``TASK_RUNNABLE`` state.

.. code-block:: c

   int sched_create_user_task(const char *name, struct process *proc,
                              struct task **out);

Allocates a user-mode task associated with ``proc``. The task's initial
userspace register state must be filled in by the caller (typically
``process_fork()`` or ``start_init()``) before the task is added to the
runqueue.

.. code-block:: c

   void sched_yield(void);

Voluntarily relinquishes the CPU. The current task remains ``TASK_RUNNABLE``
and is moved to the tail of the runqueue.

.. code-block:: c

   void sched_exit_current(int status);

Marks the current task ``TASK_ZOMBIE`` with ``exit_status = status`` and
yields. The task will not be selected again by the scheduler.

.. code-block:: c

   void sched_sleep_current(void);

Moves the current task to ``TASK_SLEEPING`` and yields. The task must be
woken by a call to ``sched_wake()`` from another context (typically an IRQ
handler or a sibling kernel thread) before it will run again.

.. code-block:: c

   void sched_wake(struct task *task);

Transitions ``task`` from ``TASK_SLEEPING`` to ``TASK_RUNNABLE`` and
appends it to the runqueue. If ``task`` has not yet called
``sched_sleep_current()``, increments ``wake_pending`` instead.

.. code-block:: c

   void sched_reap_task(struct task *task);

Frees the kernel stack and the ``struct task`` of a zombie task. The caller
must confirm that the task is not currently executing and has been removed
from all process linkage.
