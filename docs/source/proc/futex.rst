Futex
=====

A *futex* ("fast userspace mutex") is a 32-bit word in user memory that
threads can block on in the kernel when contended. musl libc builds every
``pthread`` synchronization primitive on top of ``futex(2)`` — mutexes,
condition variables, barriers, ``pthread_once``, and ``pthread_join``.

JNU implements the subset of the Linux x86_64 futex ABI that
musl's static pthread path needs: ``FUTEX_WAIT``, ``FUTEX_WAKE``, and
``FUTEX_REQUEUE``. Source: ``kernel/kernel/futex.c``,
``kernel/syscall/sys_futex.c``, ``include/jnu/kernel/futex.h``,
``include/uapi/jnu/futex.h``.

Keying
------

JNU shares memory exclusively between threads of one group
(``CLONE_VM``). A futex is therefore always **process-private**: it is
identified by the pair ``(struct process *, user virtual address)``. The
same virtual address in two different processes maps to two distinct
futexes, which is correct because those pages are not shared.

Kernel-context callers (no ``process``) use a ``NULL`` process key; they
share the kernel address space.

Data structures
---------------

Waiters are tracked in a fixed **64-bucket hash table**
(``futex_buckets[]``). Each bucket is a spinlock guarding a singly-linked
list of ``struct futex_waiter`` nodes. A waiter node lives on the
blocked task's **kernel stack** for the duration of the wait — no heap
allocation, matching the pattern in ``kernel/lib/mutex.c``.

.. code-block:: c

   struct futex_waiter {
       struct task *task;
       struct process *proc;
       uintptr_t    uaddr;
       int          woken;
       struct futex_waiter *next;
   };

The hash mixes the process pointer and the user address (low bits of the
word carry no information because futex words are 4-byte aligned).

Operations
----------

``futex_wait(uaddr, val, timeout_us)``
   Enqueue the caller on the bucket for ``(proc, uaddr)``, then compare
   ``*uaddr`` to ``val`` **under the bucket lock**. If the value does
   not match, dequeue and return ``-EAGAIN`` without sleeping — this
   closes the race where userspace checked the word, a waker ran, and the
   value changed before the syscall entered.

   If the value matches, block in ``sched_sleep_interruptible()`` (or
   ``sched_sleep_timed_interruptible()`` when ``timeout_us != 0``) until
   ``futex_wake()`` sets ``w.woken`` or the sleep is interrupted. Returns
   ``0`` when woken, ``-EINTR`` on ``TIF_NEED_DIE``, ``-ETIMEDOUT`` on
   expiry.

``futex_wake(uaddr, count)``
   Walk the bucket list; for each waiter matching ``(proc, uaddr)``,
   unlink it, set ``woken = 1``, and call ``sched_wake()``. Returns the
   number of tasks woken. ``count < 0`` is treated as wake-all
   (``INT_MAX``).

``futex_requeue(uaddr, nr_wake, nr_requeue)``
   JNU does **not** move waiters between futex queues. Instead it wakes
   up to ``nr_wake + nr_requeue`` waiters on the same word. Waking is a
   semantically safe substitute for requeuing — the woken thread
   re-contends for the target lock — at the cost of a small thundering
   herd. musl's ``pthread_cond`` unlock path relies on this.

Lost-wakeup avoidance
---------------------

The classic futex race is closed by three mechanisms working together:

1. **Enqueue before compare** — a waker that runs after enqueue but
   before sleep finds the waiter on the list and sets ``woken``.
2. **``wake_pending`` credit** — if ``sched_wake()`` fires while the
   target is still ``TASK_RUNNING``, the scheduler increments
   ``wake_pending``; the sleeper consumes the credit instead of blocking
   (see :doc:`scheduler`).
3. **Safe ``copy_from_user()`` under the bucket lock** — JNU's usercopy
   path is non-sleeping and fault-free (no demand paging), so reading
   the futex word with interrupts disabled and the bucket spinlock held
   is safe.

On timeout or ``-EINTR``, cleanup re-checks ``w.woken`` so a wake that
raced with the give-up path is not lost.

Syscall ABI
-----------

``futex`` is syscall **202** (``JNU_SYS_futex``), matching Linux
x86_64. The handler decodes the operation from the low byte of ``op``,
masking off ``FUTEX_PRIVATE`` and ``FUTEX_CLOCK_REALTIME`` (JNU has one
address space per thread group and one monotonic time source).

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - Command
     - Behavior
   * - ``FUTEX_WAIT``
     - Fourth argument is a pointer to a **relative** ``struct timespec``
       (``NULL`` = wait forever). Forwards to ``futex_wait()``.
   * - ``FUTEX_WAKE``
     - Third argument is the wake count. Forwards to ``futex_wake()``.
   * - ``FUTEX_REQUEUE``
     - Third argument is ``nr_wake``; fourth is ``nr_requeue`` (not a
       timeout). ``uaddr2`` must be valid but waiters are not moved to
       it — see ``futex_requeue()`` above.
   * - Other ops
     - Return ``-ENOSYS`` (``FUTEX_FD``, ``FUTEX_LOCK_PI``, etc.).

Misaligned ``uaddr`` values and addresses outside the user range return
``-EINVAL`` or ``-EFAULT`` as appropriate.

Thread exit and ``pthread_join``
---------------------------------

``set_tid_address(2)`` stores a ``clear_child_tid`` pointer on the
calling task. When the thread exits, ``clear_child_tid()`` in
``kernel/kernel/clone.c`` writes ``0`` to that user word and calls
``futex_wake(addr, 1)`` so a joiner blocked in ``FUTEX_WAIT`` on the
same address is unblocked. See :doc:`process` for the wider thread-exit
path.

Timed waits and the scheduler
-----------------------------

Timed ``FUTEX_WAIT`` uses ``sched_sleep_timed_interruptible()`` (see
:doc:`scheduler`). Each sleeping task may carry a ``sleep_deadline_us``
TSC deadline; ``sched_tick()`` promotes expired sleepers to runnable and
sets ``sleep_timed_out``. ``sched_consume_wake_pending()`` drops a
``wake_pending`` credit when a futex waiter observes a wake without
having entered the sleep path.

Selftest
--------

``futex_selftest()`` (registered in :doc:`/infra/selftest`) maps a scratch
user page at a fixed VA and verifies:

* misaligned addresses are rejected;
* ``futex_wake`` on an idle word returns 0;
* value mismatch returns ``-EAGAIN`` without blocking;
* a matching value with a short timeout returns ``-ETIMEDOUT``.

A full blocking wake/wait pair requires a second task and is exercised
end-to-end by musl pthread programs.

Related docs
------------

* Syscall table entry: :doc:`/syscall/table`
* Scheduler sleep/wake primitives: :doc:`scheduler`
* Thread groups and ``clear_child_tid``: :doc:`process`
