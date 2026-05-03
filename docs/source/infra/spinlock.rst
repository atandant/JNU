Spinlock
========

The spinlock primitive provides mutual exclusion and interrupt safety. In
the single-CPU build it is implemented as an IRQ-disabling wrapper; the
``xchg``/``pause`` spinning logic is a no-op because no other CPU can
preempt. The API is stable: the SMP implementation will be a drop-in
replacement.

Data Structure
--------------

.. code-block:: c

   struct spinlock {
       volatile uint32_t locked;
   };

   #define SPINLOCK_INITIALIZER { .locked = 0 }

``locked`` is zero when the lock is available and 1 when held. In the
single-CPU build, the ``xchg`` CAS is omitted; the only observable effect
is the surrounding ``cli``/``sti`` pair.

API
---

.. code-block:: c

   void spin_lock_init(struct spinlock *lock);

Initializes ``lock->locked`` to 0. Equivalent to assigning
``SPINLOCK_INITIALIZER``.

.. code-block:: c

   uint64_t spin_lock_irqsave(struct spinlock *lock);

Acquires the lock with interrupts disabled. The RFLAGS register is saved
before ``cli`` and returned to the caller. The caller must pass this value
back to ``spin_unlock_irqrestore()`` to restore the interrupt state
correctly, even if interrupts were already disabled at the call site.

.. code-block:: c

   void spin_unlock_irqrestore(struct spinlock *lock, uint64_t flags);

Releases the lock and restores RFLAGS from ``flags``. If the IF bit was
clear in ``flags`` (interrupts were already disabled before the lock was
acquired), interrupts remain disabled after the unlock.

Usage Pattern
-------------

.. code-block:: c

   struct spinlock my_lock = SPINLOCK_INITIALIZER;

   uint64_t flags = spin_lock_irqsave(&my_lock);
   /* critical section */
   spin_unlock_irqrestore(&my_lock, flags);

.. warning::

   Spinlocks must not be held across any operation that may sleep or call
   ``sched_yield()``. In the current single-CPU build this is a kernel bug
   and will cause a deadlock on the first interrupt that tries to acquire
   the same lock.

Selftests
---------

``spinlock_selftest()`` acquires and releases the lock in a nested pattern
to verify that RFLAGS are correctly saved and restored across nested
critical sections.
