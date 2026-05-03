Compiler Attributes and Portability Macros
==========================================

All compiler-specific attributes, branch-prediction hints, and utility
macros are centralized in ``include/jnu/compiler.h``. The rest of the
kernel uses these macros rather than raw GCC/Clang attribute syntax,
keeping the code readable and making future compiler ports straightforward.

Attributes
----------

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Macro
     - Expands to / Effect
   * - ``__packed``
     - ``__attribute__((packed))`` — disables struct padding. Used for
       hardware-layout structs such as CPIO headers, ACPI tables, and
       GDT descriptors.
   * - ``__aligned(x)``
     - ``__attribute__((aligned(x)))`` — enforces an alignment of ``x``
       bytes on a variable or struct. Used for the GDT (16-byte),
       TSS, and page-table pages (4096-byte).
   * - ``__noreturn``
     - ``__attribute__((noreturn))`` — marks a function as never
       returning. Applied to ``panic()``, ``panic_with_state()``, and
       ``sched_exit_current()``. Enables better dead-code elimination
       and suppresses "control reaches end of non-void function" warnings.
   * - ``__used``
     - ``__attribute__((used))`` — prevents the linker or compiler from
       discarding a symbol even if it appears unreferenced. Applied to
       Limine request objects in ``.limine_requests``.
   * - ``__unused``
     - ``__attribute__((unused))`` — suppresses unused-variable warnings
       for variables that are intentionally declared but not always used.
   * - ``__section(s)``
     - ``__attribute__((section(s)))`` — places the annotated object
       in linker section ``s``. Used to place Limine requests in
       ``.limine_requests``, ``.limine_requests_start``, and
       ``.limine_requests_end``.
   * - ``__weak``
     - ``__attribute__((weak))`` — marks a symbol as weak, allowing it
       to be overridden by a strong definition at link time.
   * - ``__printf(a, b)``
     - ``__attribute__((format(printf, a, b)))`` — enables compile-time
       format-string checking. The compiler checks argument ``b`` (the
       first variadic argument) against the format string in argument
       ``a``. Applied to ``printk()``, ``snprintf()``, ``panic()``, and
       similar functions.
   * - ``__must_check``
     - ``__attribute__((warn_unused_result))`` — issues a warning if the
       caller discards the return value. Applied to functions where
       ignoring the error code is a likely bug.

Branch Prediction Hints
-----------------------

.. code-block:: c

   #define likely(x)   __builtin_expect(!!(x), 1)
   #define unlikely(x) __builtin_expect(!!(x), 0)

These macros inform the compiler's branch predictor. ``likely(x)``
asserts that the condition ``x`` is expected to be true in the common
case; ``unlikely(x)`` asserts the opposite. They are used throughout
the hot paths of the scheduler, page-fault handler, and syscall dispatcher
to guide the compiler toward generating the fast path as the fall-through
branch.

Memory Barriers
---------------

.. code-block:: c

   #define barrier() __asm__ __volatile__("" ::: "memory")

``barrier()`` is a compiler memory barrier. It prevents the compiler from
reordering loads and stores across the barrier. It does **not** emit any
hardware instruction and is not a CPU memory fence (``mfence``). It is
used in the spinlock implementation and in any code that manipulates
shared data under a ``cli``/``sti`` guard.

Utility Macros
--------------

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Macro
     - Description
   * - ``ARRAY_SIZE(a)``
     - Returns the number of elements in a statically allocated array.
       Evaluated at compile time via ``sizeof(a) / sizeof((a)[0])``.
       Must not be applied to a pointer.
   * - ``MIN(a, b)``
     - Returns the lesser of ``a`` and ``b``. Note that ``a`` and ``b``
       are each evaluated once, so this is safe for non-side-effecting
       expressions but unsafe for expressions with side effects.
   * - ``MAX(a, b)``
     - Returns the greater of ``a`` and ``b``. Same caveats as ``MIN``.
