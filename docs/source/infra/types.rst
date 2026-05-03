Type System
===========

All kernel-internal types are defined in ``include/jnu/types.h``. The
kernel is freestanding and does not include host libc headers. Fixed-width
types are defined explicitly rather than relying on compiler builtins or
``<stdint.h>``.

Primitive Types
---------------

.. list-table::
   :header-rows: 1
   :widths: 20 30 50

   * - Type
     - Underlying type
     - Use
   * - ``int8_t``
     - ``signed char``
     - 8-bit signed integer.
   * - ``uint8_t``
     - ``unsigned char``
     - 8-bit unsigned integer. Used for byte arrays and I/O.
   * - ``int16_t``
     - ``signed short``
     - 16-bit signed integer.
   * - ``uint16_t``
     - ``unsigned short``
     - 16-bit unsigned integer. GDT selectors, mode fields.
   * - ``int32_t``
     - ``signed int``
     - 32-bit signed integer.
   * - ``uint32_t``
     - ``unsigned int``
     - 32-bit unsigned integer. LAPIC register words, PCI config space.
   * - ``int64_t``
     - ``signed long long``
     - 64-bit signed integer. Syscall return values.
   * - ``uint64_t``
     - ``unsigned long long``
     - 64-bit unsigned integer. Physical and virtual addresses, PTE words.
   * - ``size_t``
     - ``unsigned long``
     - Unsigned size type. 8 bytes on x86_64.
   * - ``ssize_t``
     - ``signed long``
     - Signed size type. Used for read/write return values.
   * - ``uintptr_t``
     - ``unsigned long``
     - Integer type that can hold any pointer value.
   * - ``intptr_t``
     - ``signed long``
     - Signed pointer-sized integer.
   * - ``ptrdiff_t``
     - ``signed long``
     - Difference between two pointers.

Address Types
-------------

Two typedef aliases distinguish physical from virtual addresses at the type
level:

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Type
     - Description
   * - ``paddr_t``
     - A physical address. Must not be dereferenced directly; requires
       conversion to a virtual address via ``phys_to_virt()`` before
       access.
   * - ``vaddr_t``
     - A virtual address. Used in VMM and paging API signatures to
       distinguish virtual from pointer types.

Both are ``uint64_t`` aliases. No implicit conversion between ``paddr_t``
and ``vaddr_t`` is enforced by the type system; discipline is by convention.

Boolean Type
------------

``bool`` is defined as ``_Bool``. ``true`` and ``false`` are defined as
integer constants 1 and 0. The kernel does not include ``<stdbool.h>``.

Static Assertions
-----------------

``types.h`` includes five ``_Static_assert`` checks that fire at compile
time if any type size deviates from the expected value. These assertions
guard against misconfigured freestanding toolchain environments:

.. code-block:: c

   _Static_assert(sizeof(uint8_t)  == 1, "uint8_t size");
   _Static_assert(sizeof(uint16_t) == 2, "uint16_t size");
   _Static_assert(sizeof(uint32_t) == 4, "uint32_t size");
   _Static_assert(sizeof(uint64_t) == 8, "uint64_t size");
   _Static_assert(sizeof(void *)   == 8, "x86_64 pointer size");
