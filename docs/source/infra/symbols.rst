Symbol Table
============

The kernel symbol table provides name-to-address and address-to-name
mapping for all exported kernel symbols. It is used primarily by the panic
subsystem to annotate RIP values and backtrace frames with human-readable
function names.

Generation
----------

The symbol table is **not** hand-maintained. It is generated automatically
by ``scripts/gen-symbols.sh`` from the linked kernel ELF after each build.
The script reads the ELF symbol table using ``nm`` or ``llvm-nm``, filters
for function and object symbols, sorts by address, and emits a C source
file (``kernel/kernel/symbols.c``) that defines ``jnu_symbols[]`` and
``jnu_symbols_count``.

This means the symbol table is always consistent with the current binary.
It is regenerated as part of the normal build process.

Data Structures
---------------

.. code-block:: c

   struct ksymbol {
       uint64_t    addr;
       const char *name;
   };

   extern const struct ksymbol jnu_symbols[];
   extern const size_t         jnu_symbols_count;

``jnu_symbols`` is a statically allocated, address-sorted array. The
``name`` pointers reference a parallel string table in the same object
file.

Lookup
------

.. code-block:: c

   bool symbols_lookup(uint64_t addr, const char **name, uint64_t *offset);

Performs a binary search over ``jnu_symbols`` to find the symbol whose
``addr`` is the largest value less than or equal to ``addr``. On success:

- ``*name`` is set to the symbol name string.
- ``*offset`` is set to ``addr - sym.addr`` (the byte offset within the
  function body).
- Returns ``true``.

Returns ``false`` if ``addr`` precedes all known symbols or if the symbol
table is empty.

.. note::

   ``symbols_lookup()`` finds the symbol that *covers* a given address,
   not an exact match. A return address pointing into the middle of a
   function will resolve to that function's name with a non-zero offset.
   This is the expected behavior for backtracing.

Usage in the Panic Path
-----------------------

``symbols_lookup()`` is called in two places within ``kernel/kernel/panic.c``:

1. When printing the faulting ``RIP`` line:
   ``RIP=0x<addr>   <name+offset>``
2. For each frame in the frame-pointer backtrace:
   ``#N  0x<addr>   <name+offset>``

If the symbol table is absent (e.g., during early bring-up before the
table is generated), ``symbols_lookup()`` returns ``false`` and the panic
path prints ``(?)`` in place of the symbol.
