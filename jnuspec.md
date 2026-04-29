# JNU Specification — v0.0.1

> **JNU** — *J is not Unix.* A monolithic, x86_64, freestanding kernel
> written from scratch in GNU C17 + Intel-syntax NASM, booted by Limine,
> licensed GPLv2.
>
> This document is the contract for v0.0.1 and the master prompt for its
> implementation. Every decision recorded here is the result of a
> deliberate debate; do not silently deviate. If a section disagrees with
> reality on the ground, fix the spec or fix the code, never neither.

---

## 1. Identity and scope

| Property         | Value                                                |
| ---------------- | ---------------------------------------------------- |
| Name             | JNU (J is not Unix)                                  |
| Version          | 0.0.1                                                |
| Architecture     | x86_64 only                                          |
| Bootloader       | Limine (Limine Boot Protocol)                        |
| Languages        | GNU C17, Intel-syntax NASM                           |
| Compiler         | clang (`--target=x86_64-unknown-none-elf`)           |
| Linker           | lld                                                  |
| Runtime builtins | compiler-rt                                          |
| Kernel design    | Monolithic with strict subsystem boundaries          |
| License          | GPLv2                                                |
| Host OS          | Windows 11 + WSL2 (Ubuntu)                           |
| Test target      | `qemu-system-x86_64 -machine q35 -m 256M`            |

JNU is **from scratch**. No Unix code is copied or adapted. The name is
not a marketing line; it is a constraint.

---

## 2. Locked decisions (the contract)

These were debated and resolved. They are not open for casual revision.

### 2.1 Kernel design

- **Monolithic.** All subsystems live in one address space, called
  through direct function calls.
- **Strict subsystem boundaries.** No subsystem reaches into another's
  internal structs. APIs are exported through one header per subsystem
  (`include/jnu/<subsys>.h`). Internal helpers stay `static` and absent
  from any header.
- **No driver model in v0.0.1.** Two abstractions only:
  `struct block_device` and `struct char_device`. Other drivers call
  each other directly. A real driver model arrives only when forced.

### 2.2 Memory management

| Layer        | Algorithm                                                       |
| ------------ | --------------------------------------------------------------- |
| PMM          | Buddy allocator, 11 orders (4 KiB → 4 MiB), zoned (DMA / NORMAL) |
| VMM          | x86_64 4-level paging, per-process PML4, kernel half shared      |
| Direct map   | HHDM provided by Limine, 2 MiB hugepages                         |
| Heap         | Slab caches + power-of-2 kmalloc, fallback to buddy for >page    |
| VMA tracking | Red-black tree per address space                                 |

### 2.3 Address space layout

```
0x0000_0000_0000_0000  user space bottom (NULL never mapped)
0x0000_0000_0040_0000  typical user load address (v0.0.2+)
0x0000_7FFF_FFFF_FFFF  user space top (128 TiB)

      ... non-canonical hole ...

0xFFFF_8000_0000_0000  HHDM (Higher-Half Direct Map, Limine)
0xFFFF_9000_0000_0000  kernel heap (slab + kmalloc)
0xFFFF_A000_0000_0000  vmalloc region (non-contiguous virtual mappings)
0xFFFF_FFFF_8000_0000  kernel image (.text/.rodata/.data/.bss, mcmodel=kernel)
```

### 2.4 CPU and interrupts

- **GDT order** (fixed, required for future `sysret`):
  null, kernel CS, kernel DS, user DS, user CS, TSS-low, TSS-high.
- **TSS in long mode**: 16-byte system descriptor. RSP0 maintained on
  every kernel-stack rebind. IST stacks for `#DF`, `#NMI`, `#MC`, `#PF`,
  each 16 KiB with a guard page.
- **IDT**: 256 vectors, all interrupt gates (IF cleared on entry).
  Macro-generated stubs handle error-code asymmetry (vectors 8, 10, 11,
  12, 13, 14, 17, 21, 29, 30 push hardware error codes; others push a
  fake zero). `swapgs` only when `(saved_cs & 3) == 3`.
- **Interrupt controllers**: legacy 8259 PIC remapped to 0x20–0x2F and
  fully masked; LAPIC + IOAPIC are the live controllers. xAPIC mode
  (MMIO). x2APIC deferred.
- **CPU features required**: long mode, APIC, MSR, TSC, NX. Set
  `EFER.NXE`, `CR0.WP`, `CR4.PGE`, `CR4.SMEP` (if present),
  `CR4.SMAP` (if present). Build with `-mno-sse -mno-mmx -mno-sse2
  -mgeneral-regs-only` until v0.0.3.
- **Vector layout**:

  | Range      | Use                                          |
  | ---------- | -------------------------------------------- |
  | 0–31       | Architectural exceptions                     |
  | 32         | LAPIC timer (used in v0.0.2; PIT in v0.0.1) |
  | 33         | PS/2 keyboard (IOAPIC pin 1, with overrides) |
  | 34         | COM1 serial RX                               |
  | 35–253     | Free                                         |
  | 254        | Reschedule IPI (SMP, future)                 |
  | 255        | LAPIC spurious                               |

### 2.5 Concurrency

- **One spinlock primitive** (`struct spinlock`). On single CPU it
  saves and restores IRQ flags around `cli`/`sti`. On future SMP it
  becomes a real `xchg`/`pause` spinloop without API change.
- **No mutexes in v0.0.1** (no scheduler to block on).
- **All interrupt handlers run with IF cleared** (interrupt gates).

### 2.6 Scheduler

- **v0.0.1: stub.** `kernel_main` runs to completion, then idle:
  `for (;;) asm volatile ("sti; hlt; cli");`
- **v0.0.2: preemptive round-robin** via LAPIC timer, 10 ms quantum,
  single ready queue. DO NOT USE PREEMTIVE ROUND ROBIN, IT IS HORRIBLE. USE MLFQ and boosting or something. IGNORE THE SPEC SAYING ROUND ROBIN.
- **Long-term destination: MLFQ** with 4–5 priority levels and dynamic
  feedback. Not CFS, not EEVDF.

### 2.7 Filesystem

- **MINIX v1 filesystem, read-only**, in v0.0.1.
- VFS layer is minimal and read-only: superblock mount, inode lookup,
  directory enumeration, file read.
- Write support, multiple FS types, page cache: deferred to v0.0.2+.

### 2.8 Drivers (Tier 0, v0.0.1 set)

| Driver               | Purpose                                |
| -------------------- | -------------------------------------- |
| Limine framebuffer   | Display (text console rendered on top) |
| COM1 16550 UART      | Serial debug output                    |
| PIT 8254             | Initial timer                          |
| RTC (CMOS)           | Wall-clock date/time at boot           |
| PS/2 keyboard (i8042)| Input                                  |
| PCI (legacy 0xCF8)   | Bus enumeration                        |
| ATA PIO              | Block device for MINIX FS              |

### 2.9 ACPI

- Parse RSDP → MADT only. Use MADT for: LAPIC list, IOAPIC list, ISA
  IRQ overrides (edge/level, polarity remappings).
- FADT, HPET, MCFG, DSDT: deferred.

### 2.10 Logging

- `printk(level, fmt, ...)` is the workhorse.
- `pr_panic`, `pr_err`, `pr_warn`, `pr_info`, `pr_debug` macros for
  ergonomics (Linux `pr_*` convention).
- 64 KiB ring buffer captures every message.
- Backends register via `klog_register(struct klog_backend *)`. v0.0.1
  backends: COM1 serial, framebuffer console.
- Compile-time level filter strips `pr_debug` in release builds.
- ANSI color emitted only by backends that declare support, and only
  for `KLOG_PANIC` and `KLOG_ERR` lines (red), `KLOG_WARN` (yellow).
- Log format: `[ssss.uuuuuu] LEVEL subsys: message`.

### 2.11 Panic

- Format is clinical. No personality, no emoji, no ASCII art, no
  reassuring prose. The output is a forensic artifact.
- Sections (in order): headline, exception decode, faulting address (if
  applicable), CPU/ring/task line, RIP with symbol, all 16 GPRs in
  aligned columns, control registers (CR0/CR2/CR3/CR4), frame-pointer
  backtrace with symbols, last 32 ring-buffer lines, `System halted.`
- See §13 for the canonical format example.
- Panic-mode print path: IF cleared, no locks taken, no allocation, no
  ring-buffer queueing — direct write to backends.

### 2.12 Testing

- **Boot-time selftests** as primary. Always compiled in; runtime-gated
  on kernel cmdline `selftest=1`.
- Each subsystem owns `<subsys>_selftest()`. Tests are fast sanity +
  short stress (RAM-bounded, ≤ a few MiB per test).
- `make test` boots the kernel in QEMU with `selftest=1`, captures
  serial output, fails on `[FAIL]` or panic markers, with a 30 s
  timeout.
- Host testing reserved for `lib/` utilities only (string, printk
  formatter, rbtree, MINIX on-disk parser). Anything that touches
  paging, interrupts, MMIO, or MSRs is host-test-forbidden.

### 2.13 Build

- Plain `make`. Helper `.sh` scripts for setup, image creation, and
  QEMU launch live in `scripts/`.
- No CMake, no autoconf, no meson, no Bazel.
- `git rev-parse --short HEAD` baked into the binary as `jnu_build[]`,
  printed on every boot alongside `JNU_VERSION`.

### 2.14 Kernel command line

- Limine passes a string. JNU parses `key=value` pairs, space-separated.
- v0.0.1 honors: `selftest=1`, `loglevel=N` (0–4), `quiet`, `debug`.

### 2.15 Debugging methodology

In priority order:

1. QEMU + gdb stub (`qemu -s -S`, `target remote :1234`).
2. Serial `printk` over COM1 (`-serial stdio`).
3. Linus-style `cli; hlt; jmp $` markers — last resort, only when (1)
   and (2) cannot reach (early boot mode-switches, page-table
   experiments that break printk's own backing memory, triple-fault
   bisection). Always `cli; hlt; jmp $`, never bare `jmp $`.

---

## 3. Coding style

JNU adopts the **Linux Kernel Coding Style** as documented in
`Documentation/process/coding-style.rst` of the Linux source, with the
additions and clarifications below. When this document is silent,
Linux's style applies.

### 3.1 Indentation and whitespace

- **Hard tabs**, displayed as 8 columns. No spaces for indentation.
- Maximum line length: **100 columns**. (Linux now allows 100; we adopt
  the modern limit, not the historical 80.)
- One statement per line. One declaration per line.
- One blank line between functions. Two blank lines never.
- No trailing whitespace. Files end with a single newline.

### 3.2 Braces

- Functions: opening brace on the **next** line.
- Control flow (`if`, `for`, `while`, `switch`, `do`): opening brace on
  the **same** line.
- A single-statement body of `if`/`for`/`while` may omit braces only if
  the body is genuinely single-statement and not part of an `if`/`else`
  chain where any other arm is braced. When in doubt, brace.

```c
int pmm_alloc_pages(int order)
{
    if (order < 0 || order >= PMM_MAX_ORDER) {
        return -EINVAL;
    }
    /* ... */
}
```

### 3.3 Naming

- **Functions**: `subsys_verb_noun` snake_case.
  Examples: `pmm_alloc_pages`, `vmm_map`, `vfs_read`, `slab_create`.
- **Structs**: `subsys_thing` snake_case. No `_t`, no `_struct`.
  Examples: `struct pmm_zone`, `struct vma`, `struct task`.
- **Typedefs**: only for primitive types and opaque handles, with `_t`.
  Examples: `paddr_t`, `vaddr_t`, `pid_t`. Do **not** typedef structs.
- **Macros and constants**: `SCREAMING_SNAKE_CASE`.
  Examples: `PAGE_SIZE`, `KERNEL_BASE`, `KLOG_INFO`.
- **Locals**: short, lowercase. `pte`, `pgd`, `vma`, `cur`. Loop
  counters `i`, `j`, `k`. No Hungarian notation.

### 3.4 Identifier hygiene (the no-noise rules)

These are **bans**, not preferences:

- ❌ No leading underscores (`_foo`, `__bar`). Reserved by the C
  standard.
- ❌ No trailing underscores on field names.
- ❌ No `_t` on types we define ourselves (only on primitive
  typedefs).
- ❌ No `_struct` suffix.
- ❌ No double-underscore prefixes for "internal". Use `static`.
- ❌ No `m_`, `g_`, `s_` Hungarian-style prefixes.
- ❌ No abbreviations that drop vowels gratuitously (`pmm_alloc_pgs` —
  no; `pmm_alloc_pages` — yes). Two exceptions, by convention: `pgd`,
  `pte` (well-established kernel terms).

### 3.5 Acronyms

Acronyms in identifiers are **lowercase** (Linux convention):
`vmm_map`, `cpu_id`, `apic_eoi`, `ioapic_init` — not `VMM_map`,
`CPU_id`, `APIC_eoi`.

### 3.6 `goto` for cleanup — required idiom

`goto` is the canonical kernel error-handling pattern and JNU embraces
it. Use it for **rollback in multi-step initialization**. Never for
forward jumps inside ordinary control flow.

```c
int subsys_init(void)
{
    int err;

    err = step_one();
    if (err) {
        goto fail_one;
    }

    err = step_two();
    if (err) {
        goto fail_two;
    }

    err = step_three();
    if (err) {
        goto fail_three;
    }

    return 0;

fail_three:
    undo_step_two();
fail_two:
    undo_step_one();
fail_one:
    return err;
}
```

Rules:
- Labels are named `fail_<step>` or `out`, lowercase.
- Cleanup labels run in reverse order of acquisition.
- One return path for success, one set of cleanup labels for failure.
- Never `goto` forward, only to cleanup at the end of the function.
- Never `goto` out of a function or across function boundaries.

### 3.7 Comments

See §4. Comments are an engineering deliverable, not decoration.

### 3.8 Switch statements

- `case` aligns with `switch`.
- Every `case` ends in `break`, `return`, `goto`, or an explicit
  `/* fallthrough */` comment (clang understands this; we also accept
  `__attribute__((fallthrough))` if required).
- `default:` is mandatory in every switch over an enum unless every
  enumerator is handled and the compiler will complain on extension.

### 3.9 Headers

- One header per subsystem: `include/jnu/<subsys>.h`.
- Include guards use `#pragma once`. (clang and gcc both support it
  reliably; we don't need the historical `#ifndef X_H` dance.)
- Headers expose only the public API. Internal helpers stay in `.c`
  files as `static`.
- No transitive includes: every `.c` and `.h` file `#include`s exactly
  what it directly uses. No relying on a header to drag in another.

### 3.10 Forbidden constructs

- ❌ `malloc`, `free`, `printf`, `memcpy`, etc. from libc. We are
  freestanding; libc does not exist. Use `kmalloc`, `kfree`, `printk`,
  `memcpy` from `lib/string.c`.
- ❌ Floating-point in kernel code (until v0.0.3). No `float`, no
  `double`, no SSE intrinsics.
- ❌ VLAs (variable-length arrays). Always fixed size or heap.
- ❌ Implicit narrowing conversions; use explicit casts when narrowing.
- ❌ Magic numbers. Use a `#define` or `enum` and explain the source
  (datasheet section, Intel SDM volume).
- ❌ `#ifdef` mazes. Architecture-conditional code lives under
  `arch/x86_64/`, not behind preprocessor walls in shared files.

---

## 4. Commenting policy

Comments are not optional and not decorative. They explain decisions
that the code cannot, and they protect future-you from past-you.

### 4.1 The fundamental rule

**Comments explain *why*. Code explains *what*.** If a comment
restates the code, delete one of them — usually the comment, sometimes
the code (because it's unclear).

Bad:
```c
i = i + 1;  /* increment i */
```

Good:
```c
/*
 * Skip the legacy ISA hole (640 KiB – 1 MiB). BIOS may have left
 * shadow ROM mappings here that look like normal RAM in the memory
 * map but page-fault on first access. See Intel SDM Vol. 3 §11.4.
 */
if (region->base < 0x100000 && region->base + region->len > 0xA0000) {
    region->len = 0xA0000 - region->base;
}
```

### 4.2 File header comment

Every `.c` and `.h` file begins with:

```c
/*
 * <relative path> — <one-line purpose>
 *
 * <2–6 line description: what this file does, what it depends on, what
 *  invariants it maintains, what it deliberately does not do.>
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */
```

Example:

```c
/*
 * mm/pmm.c — Physical memory manager (buddy allocator).
 *
 * Manages physical page frames in 4 KiB granularity, organized into
 * 11 buddy orders (4 KiB to 4 MiB). Two zones: ZONE_DMA covers the
 * first 16 MiB for legacy ISA DMA; ZONE_NORMAL covers the rest.
 *
 * This file does NOT manage virtual mappings — see mm/vmm.c.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */
```

### 4.3 Function comments

Every non-`static` function and every `static` function longer than
~20 lines gets a comment block of this form:

```c
/*
 * <verb-phrase one-line summary>.
 *
 * <Optional 1–4 lines of context: what this is for, when callers use
 *  it, how it relates to neighboring functions.>
 *
 * Parameters:
 *   <name>  <semantic, units, ownership, valid range>
 *
 * Returns:
 *   <success value> on success, <negative errno> on failure.
 *
 * Locking:
 *   <which locks must / must not be held by the caller>.
 *
 * Context:
 *   <interrupt-safe? sleep-safe? called from panic path?>
 */
```

Drop sections that are trivial. A pure helper that takes two integers
and returns one does not need a `Locking:` block; a function that
touches kernel data structures does.

### 4.4 Inline comments

Reserve inline comments for the cases where reading the code does not
tell you why the line exists:

- Hardware quirks (`/* errata: AMD Fam 17h needs this STRONG fence */`).
- ABI requirements (`/* SysV: 16-byte aligned RSP at call */`).
- Magic numbers that must reference a source (`/* Intel SDM 3-25 §10.4.4 */`).
- Non-obvious orderings (`/* must run before vmm_init: enables NX */`).
- Deliberate non-idiomatic constructs (`/* cannot use memcpy: source is MMIO */`).

### 4.5 TODO / FIXME / XXX

- `TODO(name): <what>` — planned work, not yet a bug.
- `FIXME(name): <what>` — known bug or known incomplete handling.
- `XXX(name): <what>` — danger sign, deserves review before relying.

The `(name)` is a short identifier of the author or issue, so the
comment can be traced. Bare `TODO`/`FIXME` without attribution rots
fastest.

### 4.6 What to never comment

- Auto-generated boilerplate ("constructor for foo struct").
- Restating types that are already in the signature.
- Cargo-cult disclaimers ("this code is dangerous, be careful").
- Banner separators (`/* ============= */`) — file headers and function
  comments are enough structure.

---

## 5. Repository layout

```
JNU/
├── jnuspec.md                   # this file
├── README.md
├── LICENSE                      # GPLv2
├── Makefile                     # top-level build orchestration
├── scripts/
│   ├── setup-wsl.sh             # one-shot toolchain bootstrap
│   ├── make-image.sh            # build bootable ISO with Limine
│   ├── make-disk.sh             # build MINIX FS disk image
│   ├── run-qemu.sh              # boot in QEMU
│   ├── debug-qemu.sh            # boot QEMU with -s -S for gdb
│   └── gen-symbols.sh           # extract symbol table from kernel ELF
├── boot/
│   ├── limine.cfg               # Limine config (timeout, kernel path, cmdline)
│   └── limine/                  # Limine binaries (vendored, pinned version)
├── kernel/
│   ├── arch/x86_64/
│   │   ├── boot.S               # Limine entry → long-mode C
│   │   ├── isr.S                # 256 stubs + isr_common
│   │   ├── gdt.c                # GDT + TSS + IST setup
│   │   ├── idt.c                # IDT build + load
│   │   ├── exceptions.c         # exception handler bodies
│   │   ├── apic.c               # LAPIC + IOAPIC
│   │   ├── pic.c                # legacy PIC: remap + mask
│   │   ├── cpu.c                # CPUID, CR0/CR4/EFER, per-CPU init
│   │   └── paging.c             # 4-level page table operations
│   ├── mm/
│   │   ├── pmm.c                # buddy allocator
│   │   ├── vmm.c                # address space operations
│   │   ├── slab.c               # slab + kmalloc
│   │   └── vma.c                # VMA red-black tree
│   ├── drivers/
│   │   ├── serial.c             # COM1 16550 UART
│   │   ├── pit.c                # 8254 timer
│   │   ├── rtc.c                # CMOS RTC
│   │   ├── kbd.c                # PS/2 keyboard via i8042
│   │   ├── fbcon.c              # framebuffer text console
│   │   ├── pci.c                # PCI enumeration (legacy I/O ports)
│   │   └── ata.c                # ATA PIO
│   ├── fs/
│   │   ├── vfs.c                # minimal read-only VFS
│   │   ├── block.c              # block_device abstraction
│   │   └── minix.c              # MINIX FS reader
│   ├── lib/
│   │   ├── string.c             # memcpy, memset, memcmp, strlen, strcmp
│   │   ├── printk.c             # vsnprintf-style formatter, ring buffer, klog
│   │   ├── rbtree.c             # generic red-black tree
│   │   └── spinlock.c           # IRQ-disable spinlock (single-CPU shim)
│   ├── kernel/
│   │   ├── main.c               # kernel_main
│   │   ├── panic.c              # panic + register dump + backtrace
│   │   ├── sched.c              # scheduler stub
│   │   ├── cmdline.c            # kernel cmdline parser
│   │   └── symbols.c            # generated symbol table (in build dir)
│   └── include/jnu/
│       ├── compiler.h           # __packed, __noreturn, likely/unlikely, etc.
│       ├── types.h              # paddr_t, vaddr_t, size_t, bool, etc.
│       ├── errno.h              # -E* error codes
│       ├── pmm.h
│       ├── vmm.h
│       ├── slab.h
│       ├── kmalloc.h
│       ├── vma.h
│       ├── apic.h
│       ├── ioapic.h
│       ├── idt.h
│       ├── gdt.h
│       ├── cpu.h
│       ├── paging.h
│       ├── pci.h
│       ├── ata.h
│       ├── serial.h
│       ├── pit.h
│       ├── rtc.h
│       ├── kbd.h
│       ├── fbcon.h
│       ├── block.h
│       ├── chardev.h
│       ├── vfs.h
│       ├── minix.h
│       ├── klog.h
│       ├── panic.h
│       ├── spinlock.h
│       ├── rbtree.h
│       ├── string.h
│       ├── cmdline.h
│       └── selftest.h
└── tests/
    └── host/                    # host-only unit tests for lib/
        ├── Makefile
        ├── test_rbtree.c
        ├── test_printk.c
        ├── test_string.c
        └── test_minix_parse.c
```

This structure is **flat on purpose**. One `.c` per subsystem until
size or distinct concerns force a split (see the splitting rule in
§3.9 and below). One header per subsystem.

**Splitting rule**: a subsystem may be split into multiple `.c` files
only when at least one of the following is true:

1. A second algorithm implements the same interface (e.g., we one day
   add `pmm_bitmap.c` alongside `pmm_buddy.c`).
2. A piece is genuinely reusable across subsystems and lives in `lib/`.
3. The file has crossed ~1000 lines **and** has a natural seam (a layer
   boundary, a different algorithm, a different change frequency).

Splitting purely because a file "feels big" is forbidden.

---

## 6. Build system

### 6.1 Toolchain

Installed in WSL2 (Ubuntu 22.04 or newer):

```
clang >= 16        # any recent clang with x86_64-unknown-none-elf target
lld
nasm
make
qemu-system-x86_64
xorriso            # for ISO creation
mtools             # for FAT manipulation
```

`scripts/setup-wsl.sh` installs all of the above with `apt` and
verifies versions.

### 6.2 Compiler flags

Common (kernel C):

```
-target x86_64-unknown-none-elf
-std=gnu17
-ffreestanding
-fno-stack-protector
-fno-pic -fno-pie
-mno-red-zone
-mno-sse -mno-mmx -mno-sse2 -mgeneral-regs-only
-mcmodel=kernel
-fno-omit-frame-pointer
-Wall -Wextra -Wpedantic
-Wshadow -Wconversion -Wsign-conversion
-Wmissing-prototypes -Wstrict-prototypes
-Wnull-dereference
-Werror
-O2 -g3
-nostdinc
-Iinclude
```

Assembly (NASM):

```
-f elf64
-F dwarf
-g
```

Linker (lld):

```
-nostdlib -no-pie
-T kernel/arch/x86_64/linker.ld
-z max-page-size=0x1000
```

### 6.3 Targets

```
make            # build kernel ELF + bootable ISO
make iso        # bootable ISO only
make disk       # MINIX FS disk image
make run        # boot in QEMU, serial → stdio, framebuffer → window
make debug      # boot QEMU with -s -S, ready for gdb attach
make test       # boot with selftest=1, scrape serial, fail on [FAIL]
make hosttest   # build and run lib/ unit tests on host
make clean      # delete build/
make distclean  # delete build/ and vendored Limine binaries
```

### 6.4 Build output

```
build/
├── kernel.elf       # linked kernel with full symbols
├── kernel.sym       # nm-stripped symbol table for symbols.c
├── kernel.iso       # bootable ISO with Limine
├── disk.img         # MINIX FS test disk
└── obj/             # intermediate .o files mirroring kernel/ tree
```

### 6.5 Versioning and build metadata

The Makefile generates `build/obj/buildinfo.c`:

```c
const char jnu_version[] = "0.0.1";
const char jnu_build[]   = "<short-sha-or-'unknown'>";
const char jnu_buildtime[] = "<UTC ISO 8601>";
```

`kernel_main` prints all three on the first `pr_info` line of every
boot.

---

## 7. Phase 1 — Foundation

**Objective**: from `make` to a kernel that boots via Limine, prints to
COM1 and the framebuffer, and idle-loops cleanly.

### 7.1 Deliverables

- Repository skeleton, Makefile, scripts.
- `boot/limine.cfg` with kernel path, cmdline placeholder, boot timeout.
- `kernel/arch/x86_64/boot.S` — Limine entry point, sets up initial
  stack, jumps to `kernel_main` in C.
- `kernel/arch/x86_64/linker.ld` — kernel image at `0xFFFFFFFF80000000`,
  proper section layout, `.bss` cleared by Limine.
- `kernel/lib/string.c` — `memcpy`, `memset`, `memcmp`, `strlen`,
  `strcmp`, `strncmp`, `strchr`. Hand-rolled, no SSE.
- `kernel/lib/printk.c` —
  - `vsnprintf` formatter supporting `%d %u %x %X %lld %llu %llx %s %c
    %p %%`, width/padding (`%08x`, `%5d`).
  - 64 KiB ring buffer with single-writer-single-drain semantics.
  - `klog_register(struct klog_backend *)`.
  - `printk(level, fmt, ...)` and `pr_*` macros in `<jnu/klog.h>`.
- `kernel/drivers/serial.c` —
  - Initialize 16550 at COM1 (port 0x3F8, 115200 8N1).
  - Register as klog backend.
  - Polling write only in v0.0.1 (no IRQ-driven TX yet).
- `kernel/drivers/fbcon.c` —
  - Consume Limine framebuffer info.
  - Embedded PSF1 or PSF2 monospace font (e.g., 8×16 VGA-style),
    compiled in.
  - Glyph blit, scrollback by line.
  - Register as klog backend.
- `kernel/kernel/cmdline.c` — split cmdline string, store key=value
  pairs in a fixed-size table (~32 entries), expose
  `cmdline_get(key)`.
- `kernel/kernel/panic.c` — minimal panic in this phase: format
  message, write headline, `cli; hlt; jmp $`. Register dump and
  backtrace come in Phase 2.
- `kernel/kernel/main.c` — `kernel_main`:
  1. Parse cmdline.
  2. Init serial, print `JNU vX.Y.Z (build SHA, time)`.
  3. Init framebuffer console, register as klog backend.
  4. `pr_info("kernel: phase 1 boot complete\n")`.
  5. `for (;;) asm volatile ("hlt");`

### 7.2 Phase 1 success criteria

- `make run` produces a Limine boot menu, selecting JNU launches the
  kernel.
- `JNU 0.0.1 (...)` appears on both QEMU window and `-serial stdio`.
- `pr_info`, `pr_warn`, `pr_err` lines all appear with correct level
  tags and timestamps. `pr_warn` lines are yellow on serial; `pr_err`
  red. (Timestamps are fine to be `[0.000000]` in this phase — no real
  TSC calibration yet.)
- A deliberate `panic("hello panic")` call produces the headline,
  `System halted.`, and a stuck CPU.
- Kernel runs idle indefinitely without spurious output.

---

## 8. Phase 2 — CPU and memory

**Objective**: real GDT/IDT/TSS/IST, real interrupt controllers, full
memory management, full panic with backtrace, selftests.

### 8.1 Deliverables

- `kernel/arch/x86_64/cpu.c` —
  - CPUID feature detection. Panic if any required feature is missing.
  - Set `EFER.NXE`, `CR0.WP`, `CR4.PGE`, conditionally `CR4.SMEP`,
    `CR4.SMAP`.
  - TSC calibration via PIT (one-shot mode): produce a cycles-per-
    microsecond constant for klog timestamps.
  - Define `struct cpu` (per-CPU block); for v0.0.1 there is exactly
    one. Wire `IA32_GS_BASE` to point at it.
- `kernel/arch/x86_64/gdt.c` —
  - Build the 7-entry GDT (null, kCS, kDS, uDS, uCS, TSS-low, TSS-high).
  - Allocate TSS, allocate seven IST stacks (16 KiB each), put
    sentinels in unused IST slots.
  - `lgdt`, `ltr`.
- `kernel/arch/x86_64/idt.c` + `isr.S` —
  - 256 stubs generated by NASM macros. No-error stubs push fake 0;
    error stubs let the CPU's pushed code stand.
  - `isr_common` saves all 15 GPRs (skip RSP), checks `(saved_cs & 3)
    == 3` for swapgs.
  - Calls `interrupt_dispatch(struct cpu_state *)` in
    `exceptions.c`/`apic.c`.
  - All 256 vectors as interrupt gates.
- `kernel/arch/x86_64/exceptions.c` —
  - Named handlers for #DE, #DB, #NMI, #BP, #OF, #BR, #UD, #NM, #DF,
    #TS, #NP, #SS, #GP, #PF, #MF, #AC, #MC, #XF, #VE, #CP.
  - All unhandled exceptions go to `panic()` with full state.
- `kernel/arch/x86_64/pic.c` — remap to 0x20–0x2F, mask all IRQs. Done
  forever after boot.
- `kernel/arch/x86_64/apic.c` —
  - Read `IA32_APIC_BASE`, map LAPIC MMIO uncached.
  - Configure spurious vector register (vector 0xFF, enable bit).
  - `apic_eoi()` for IRQ handlers.
  - Tiny ACPI parser (just enough to find RSDP from Limine, then
    XSDT/RSDT, then MADT).
  - Parse MADT: collect LAPIC list, IOAPIC base, ISA IRQ overrides.
  - Initialize IOAPIC: mask all redirection entries, then unmask only
    those we wire up later (PS/2 keyboard, COM1 RX in Phase 3).
- `kernel/arch/x86_64/paging.c` —
  - `paging_map(space, virt, phys, flags, pages)`,
    `paging_unmap(space, virt, pages)`,
    `paging_protect(space, virt, flags, pages)`.
  - 4 KiB and 2 MiB page support (the latter for HHDM and kernel
    image).
  - `paging_clone_kernel_half(new_pml4)` shares kernel mappings via
    PDPT-table-pointer copy.
  - `invlpg` on single page changes; full CR3 reload on bulk.
- `kernel/mm/pmm.c` —
  - Buddy allocator over Limine memory map.
  - Per-zone (DMA: <16 MiB, NORMAL: rest) free lists per order.
  - API: `pmm_alloc_pages(int order)`, `pmm_free_pages(paddr_t,
    int order)`, `pmm_alloc_dma(int order)`.
  - `pmm_selftest`: alloc 1024 single pages, alloc 64 order-3 blocks,
    free in random order, assert all reclaimed.
- `kernel/mm/vmm.c` —
  - `struct addr_space` wraps a PML4 plus a VMA tree.
  - `vmm_create_space()`, `vmm_destroy_space()`,
    `vmm_switch_to(space)`.
  - `vmm_map(space, virt, phys, pages, flags)` and friends, delegating
    to `paging.c` and updating the VMA tree.
  - `vmm_selftest`: map a region, write, change protection to RO,
    assert write faults (catch in handler), unmap.
- `kernel/mm/slab.c` —
  - `kmem_cache_create(name, size, align, ctor)`,
    `kmem_cache_alloc(cache)`, `kmem_cache_free(cache, obj)`.
  - Power-of-2 size-class caches backing `kmalloc`/`kfree`.
  - For >1 page, fall through to `pmm_alloc_pages`.
  - `slab_selftest`: 5000 alloc/free across multiple classes, leak
    check.
  - note from the user, you may want to use SLUB if asked by the spec,
  - so check the spec and if that's the case. rename it to slub.c/SLUB.c
  - i prefer SLUB over slab, it's more modern.
- `kernel/mm/vma.c` —
  - Red-black tree of `struct vma` keyed on virtual start address.
  - Insert rejects overlap; lookup is O(log n).
  - Reuses `lib/rbtree.c`.
- `kernel/lib/rbtree.c` —
  - Generic red-black tree (Linux-style, with `struct rb_node` embedded
    in user types).
  - `rbtree_selftest`: insert 1000 random keys, walk in order, delete
    half, walk again, verify invariants.
- `kernel/kernel/symbols.c` —
  - Generated from `kernel.elf` via `nm`. Compiled `.c` containing a
    sorted `{addr, name}[]`.
  - `symbols_lookup(addr, &name, &offset)` — binary search.
- `kernel/kernel/panic.c` (full version) —
  - Format per §13.
  - Frame-pointer backtrace.
  - Drain last 32 ring-buffer lines.
  - Direct backend writes, no locks, no allocation.
- `kernel/kernel/sched.c` — stub. `sched_init` does nothing in v0.0.1;
  the placeholder is here so other subsystems can `#include <jnu/
  sched.h>` for forward-declared types if needed.
- `kernel/lib/spinlock.c` —
  - `spin_lock`/`spin_unlock` saving/restoring IRQ flags.
  - `spinlock_selftest`: not very interesting on a single CPU; assert
    nesting save/restore is correct.

### 8.2 Phase 2 success criteria

- A deliberate `*(volatile int *)0xdeadbeef = 0;` in test code produces
  the full canonical panic output: headline, exception decode, faulting
  address `0xdeadbeef`, register dump, backtrace with symbols, last 32
  log lines, `System halted.`
- A deliberate division by zero (`int x = 1; int y = 0; x / y;`) produces
  a #DE panic.
- A deliberate kernel stack overflow (recursive function) produces a
  #DF on the IST stack with a clean panic — no triple fault.
- `make test` (with `selftest=1`) runs every selftest; all pass.
- `meminfo` (a tiny temporary debug command exposed via cmdline or a
  serial-RX hook) prints PMM stats: total pages, free pages by order,
  zone breakdown.

---

## 9. Phase 3 — Devices and I/O

**Objective**: real timer, real input, real bus enumeration, real disk.

### 9.1 Deliverables

- `kernel/drivers/pit.c` —
  - Configure PIT channel 0 in mode 2 (rate generator) at 100 Hz.
  - Timer tick handler (vector 32) increments a 64-bit jiffies counter.
  - Used here only for TSC calibration and as a fallback monotonic
    clock; will be retired in v0.0.2 when LAPIC timer takes over.
- `kernel/drivers/rtc.c` —
  - Read CMOS RTC once at boot; parse BCD or binary based on Status
    Register B.
  - Expose `rtc_now(struct tm *)`.
  - Print boot wall time via `pr_info`.
- `kernel/drivers/kbd.c` —
  - Initialize i8042 controller: disable both ports, flush, self-test
    (0xAA), enable port 1, enable scanning, set scancode set 1.
  - IRQ handler on vector 33 reads scancode, translates to a key event
    via a static layout table (US QWERTY only in v0.0.1).
  - Internal ring buffer of `struct key_event { kbd_key, bool pressed,
    modifiers }`.
  - Implements `struct char_device` for line-buffered reads.
- `kernel/drivers/pci.c` —
  - Legacy I/O config (`0xCF8` / `0xCFC`).
  - `pci_for_each(callback)` enumerates bus 0–255, dev 0–31, func 0–7,
    skipping nonexistent.
  - `pci_read_config_*`, `pci_write_config_*`.
  - `pci_find_class(class, subclass, prog_if)` for storage/network
    discovery.
  - At boot, `pr_info` every device found: bus:dev.func vendor:device
    class subclass.
- `kernel/drivers/ata.c` —
  - Detect primary and secondary ATA channels via PCI (IDE storage
    class) or fall back to legacy ports (0x1F0 / 0x170).
  - `ata_identify` on each channel/drive position.
  - `ata_read_sectors(drive, lba, count, buf)` using PIO (no DMA in
    v0.0.1).
  - Implements `struct block_device`, registered with the block layer.
- `kernel/fs/block.c` —
  - `struct block_device` registry. Operations: `read(lba, count, buf)`,
    `write(lba, count, buf)` (no-op in v0.0.1), `ioctl` (placeholder).
  - `block_register(struct block_device *)`,
    `block_lookup(const char *name)`.
- `kernel/include/jnu/chardev.h` and minimal use in `kbd.c`,
  `serial.c`. Char device API: `read(buf, len)`, `poll()`.
- Per-driver selftests where they make sense:
  - `pci_selftest`: assert at least one device was enumerated (the
    QEMU root complex).
  - `ata_selftest`: read sector 0 (the boot sector / superblock area)
    and check its size, no panic.
  - PIT, RTC, kbd, fbcon are not selftested (they are interactive or
    purely time-driven).

### 9.2 Phase 3 success criteria

- Boot lists all PCI devices, the ATA channels, and the keyboard, in
  klog at `INFO`.
- A debug hook gated on cmdline `dump=blocks` reads sectors 0–7 of
  `disk.img` and prints them in a hex-dump format at boot.
- Pressing keys on the QEMU console shows scancodes / decoded keys via
  a debug `pr_debug("kbd: %c\n", c)` line.
- TSC-derived klog timestamps make sense (PIT calibration produces a
  reasonable cycles/µs).
- All selftests still green.

---

## 10. Phase 4 — VFS and MINIX FS

**Objective**: mount a real filesystem and walk it from boot. No
shell in v0.0.1; once initialization completes, the kernel prints a
summary line and idle-loops.

A real interactive surface — kernel-mode shell, then userspace
`init` — is deferred to a later release. The decision is deliberate:
the shell would pull in line editing, scancode-to-line buffering,
command parsing, and several FS exercises that bloat the v0.0.1
contract without changing what we have actually proven to work. We
prove the boot-to-mount path here, and stop.

### 10.1 Deliverables

- `kernel/fs/vfs.c` —
  - Read-only VFS. `struct vfs_mount`, `struct vfs_inode`,
    `struct vfs_dirent`.
  - Operations table per filesystem: `mount`, `lookup`,
    `readdir`, `read`, `getattr`.
  - `vfs_mount(blockdev_name, fstype, mount_point)`.
  - `vfs_open(path)` returns an inode handle; `vfs_read(inode, off,
    len, buf)`.
  - One mount point in v0.0.1: `/`.
  - `vfs_selftest`: open `/` from the test image, and assert it is a directory.
- `kernel/fs/minix.c` —
  - Implement MINIX v1 superblock parsing (offset 1024, magic
    `0x137F` or `0x138F`).
  - Inode table layout, indirect/double-indirect block resolution.
  - Directory entries (16-byte: 2-byte inode number, 14-byte name).
  - Block-by-block reads via `block_device`.
  - `minix_selftest`: mount the test image, verify root inode is a
    directory, count entries.
- `kernel/kernel/main.c` (final form) —
  1. Print `JNU vX.Y.Z (build SHA, time)`.
  2. Init serial, fb console, klog.
  3. Init CPU, GDT, IDT, exceptions, APIC.
  4. Init PMM, VMM, slab.
  5. Init PIT, RTC.
  6. Init PCI; init ATA; register block devices.
  7. Init keyboard.
  8. Mount `/` from the first ATA disk as MINIX FS. Panic if mount
     fails.
  9. Print a one-line root-directory summary
      (`pr_info("rootfs: %u entries\n", n)`).
  10. If `selftest=1`, run all `*_selftest()` functions and panic on
     first failure.
  11. `pr_info("kernel: boot complete; idle\n");`
  12. `for (;;) asm volatile ("sti; hlt; cli");`

There is **no `kernel/kernel/shell.c` in v0.0.1**. Reintroducing it
requires re-debating §2 and updating this section.

### 10.2 Phase 4 success criteria — these are the v0.0.1 exit criteria

JNU v0.0.1 is **complete** when, booted via Limine in QEMU with a
MINIX FS disk image:

1. Boots cleanly through every initialization step and reaches the
   idle loop on framebuffer + COM1, with no spurious output after
   `kernel: boot complete; idle`.
2. Boot output includes:
   - JNU version, build SHA, build time.
   - CPU feature summary.
   - Memory map (Limine-provided regions, PMM zone breakdown).
   - ACPI MADT summary (LAPIC count, IOAPIC base, IRQ overrides).
   - PCI device list.
   - The MINIX root-directory entry count.
3. With `selftest=1` cmdline, all selftests pass within 5 s of boot
   and the kernel still reaches the idle loop afterwards.
4. A deliberate `panic("v0.0.1 panic check")` invocation gated on a
   cmdline flag (`panictest=1`) produces a clean canonical panic
   with register dump, backtrace, and ring-buffer tail. No triple
   faults.
5. PMM and slab show no leaks across 1000 allocate/free cycles
   (asserted by their respective selftests).
6. Identical behavior on:
   - `qemu-system-x86_64 -machine q35 -m 256M` (no KVM).
   - `qemu-system-x86_64 -machine q35 -m 256M -enable-kvm` (with KVM).

When all six are green, tag `v0.0.1`.

---

## 11. Out of scope for v0.0.1 (and where it lands)

| Feature                                  | Lands in        |
| ---------------------------------------- | --------------- |
| Userspace, ring 3, ELF loader            | v0.0.2          |
| Syscalls (`syscall`/`sysret`, MSRs)      | v0.0.2          |
| Scheduler (MLFQ)                         | v0.0.2          |
| MINIX FS write support                   | v0.0.2          |
| LAPIC timer (replacing PIT)              | v0.0.2          |
| HPET                                     | v0.0.2          |
| AHCI / SATA                              | v0.0.2          |
| PS/2 mouse                               | v0.0.2          |
| FPU/SSE/AVX state save in context switch | v0.0.3          |
| First NIC (rtl8139 or virtio-net)        | v0.0.3          |
| virtio-blk                               | v0.0.3          |
| KASLR (Kernel ASLR)                      | v0.0.3          |
| Page zeroing/poisoning on `pmm_free`     | v0.0.3          |
| SMP, x2APIC, IPIs, TLB shootdown         | v0.0.4          |
| Loadable modules                         | v0.0.5+         |
| Page replacement / swap                  | v0.0.5+         |
| Network stack                            | v0.0.5+         |
| USB / xHCI                               | later           |
| NVMe                                     | later           |
| Real driver model                        | later           |
| GPU drivers, WiFi, Bluetooth, audio      | unlikely, ever  |

---

## 12. Risks and mitigations

| Risk                                                | Mitigation                                                   |
| --------------------------------------------------- | ------------------------------------------------------------ |
| TSS / IST setup wrong → triple fault on real HW     | Verified by deliberate stack-overflow test in Phase 2.        |
| ACPI MADT parser too strict → boot fails on some boxes | Limine provides RSDP; we treat unknown MADT entries as info, not errors. |
| Buddy allocator off-by-one in coalescing            | Selftest with random alloc/free in `pmm_selftest`.            |
| Ring buffer races during early boot                 | Single-CPU; printk locks are no-ops or save IRQ flags.        |
| Symbol table out of date with kernel ELF            | `gen-symbols.sh` is a Make dependency; rebuilds on every link. |
| GDT order wrong → future `sysret` breaks            | Order is asserted at compile time via `_Static_assert` on offsets. |
| Clang-only flags drift                              | Pinned clang version range in `setup-wsl.sh`; CI later.       |
| HHDM stale data scraping (information leak)         | Deferred page zeroing/poisoning to v0.0.3. Acknowledged risk vs performance tradeoff in v0.0.1. |

---

## 13. Canonical panic output (reference)

This is the exact shape Phase 2's `panic()` must produce. Spacing and
column alignment matter — diagnostic readability across terminals
depends on it.

```
[   12.045123] PANIC pmm: double-free of page 0x0000000012340000

Exception: #PF (vector 14)  error=0x0002  (write, not-present, supervisor)
Faulting address: 0x0000000000000000

CPU 0  ring 0  task=<kernel>

RIP=0xffffffff80104d2a   pmm_free_pages+0x4a/0x120
CS =0x0008  SS =0x0010   RFLAGS=0x0000000000010246
RSP=0xffffffff80300f00

RAX=0x0000000000000000   RBX=0xffffffff80300f80
RCX=0x0000000000000020   RDX=0x0000000012340000
RSI=0x0000000000000001   RDI=0x0000000012340000
RBP=0xffffffff80300f30   R8 =0x0000000000000000
R9 =0x0000000000000000   R10=0x0000000000000000
R11=0x0000000000000246   R12=0x0000000000000000
R13=0xffffffff80300fa0   R14=0xffffffff80105000
R15=0x0000000000000003

CR0=0x0000000080050033   CR2=0x0000000000000000
CR3=0x0000000000600000   CR4=0x0000000000000020

Backtrace:
  #0  0xffffffff80104d2a   pmm_free_pages+0x4a
  #1  0xffffffff80108b14   slab_free+0x84
  #2  0xffffffff80112340   vfs_cleanup+0x20
  #3  0xffffffff80100245   kernel_main+0x125
  #4  0xffffffff80100020   _start+0x10

Last log lines:
  [   11.998123] INFO  pmm: 8192 free pages, 32 MiB
  [   11.999402] INFO  slab: cache 'inode' init (size=192, align=8)
  [   12.001003] WARN  ata: device 0 slow IDENTIFY (532ms)
  [   12.045120] DEBUG pmm: free 0x0000000012340000

System halted.
```

Color rules: only the leading `PANIC` token in the headline is red;
all other content is the terminal's default color.

---

## 14. Glossary

| Term       | Meaning                                                                         |
| ---------- | ------------------------------------------------------------------------------- |
| HHDM       | Higher-Half Direct Map. Every byte of physical RAM mapped at a known kernel-virtual offset, set up by Limine. |
| LAPIC      | Local Advanced Programmable Interrupt Controller, per-CPU.                       |
| IOAPIC     | I/O APIC, chipset-level, routes external pins to LAPIC vectors.                  |
| IST        | Interrupt Stack Table, 7-entry array in TSS specifying alternate stacks for specific exception vectors. |
| PMM        | Physical Memory Manager — page frame allocator.                                  |
| VMM        | Virtual Memory Manager — page tables and address spaces.                         |
| VMA        | Virtual Memory Area — a contiguous region with shared permissions in an address space. |
| PML4       | Page Map Level 4, top-level page table in x86_64 4-level paging.                 |
| MADT       | Multiple APIC Description Table, an ACPI table.                                  |
| MSR        | Model-Specific Register.                                                         |
| TSC        | Time Stamp Counter, CPU cycle counter.                                           |
| TSS        | Task State Segment, used in long mode mainly for RSP0 and IST.                   |
| PIO        | Programmed I/O — CPU executes every byte of transfer.                            |
| MINIX FS   | The MINIX filesystem (v1 in v0.0.1), a small Unix-style filesystem.              |
| Selftest   | A `<subsys>_selftest()` function that exercises a subsystem at boot when `selftest=1`. |

---

## 15. Document discipline

- This file is the source of truth. If implementation diverges,
  reconcile by updating the file *or* the code, not by leaving them
  inconsistent.
- Every phase is sequential. Do not start Phase 2 work in Phase 1
  without recording the dependency reversal here.
- Every locked decision in §2 has a debate record in the project's
  thread history. Do not unlock without re-debating.
