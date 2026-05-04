# JNU Specification — v0.0.3

> **JNU** — *J is not Unix.* A monolithic, x86_64, freestanding kernel
> written from scratch in GNU C17 + Intel-syntax NASM, booted by Limine,
> licensed GPLv2.
>
> This document is the contract for v0.0.3 and the master prompt for its
> implementation. v0.0.3 is the **"real userspace runs" release**: the
> kernel grows enough machinery that a statically-linked, musl-libc
> userspace binary built by an unmodified clang toolchain runs to
> completion.
>
> v0.0.3 is a **major release**. It supersedes specific clauses of
> `jnuspec2.md` (called out below) and inherits everything else from
> the spec chain `jnuspec022.md → jnuspec021.md → jnuspec2.md →
> jnuspec.md`. If something is not mentioned here, the parent chain
> still applies.
>
> Do not silently deviate. If this document disagrees with reality on
> the ground, fix the spec or fix the code, never neither.

---

## 1. Identity and scope

| Property         | Value                                                     |
| ---------------- | --------------------------------------------------------- |
| Name             | JNU (J is not Unix)                                       |
| Version          | 0.0.3                                                     |
| Parent spec      | `jnuspec022.md` (chains to `jnuspec021.md`, `jnuspec2.md`, `jnuspec.md`) |
| Architecture     | x86_64 only                                               |
| Process model    | `fork` + `execve`, CoW (inherited from v0.0.2.2)          |
| Memory model     | `mmap` / `munmap` / `mprotect` (anonymous private)        |
| Userspace ABI    | **Linux x86_64-compatible numbers** for shared syscalls   |
| FPU/SSE          | Eager XSAVE/FXSAVE, x87 + SSE only                        |
| License          | GPLv2                                                     |

The release ships exactly five themes:

1. **mmap/munmap/mprotect** for anonymous private mappings, with
   lazy zero-fill and zero-page CoW.
2. **Eager FPU/SSE state save** on every context switch.
3. **Page zero-on-free** in the PMM.
4. **execve refactored** onto a single `vmm_map_anonymous` helper that
   also backs `mmap`, plus a guard page below the user stack.
5. **Linux-compatible syscall numbering**, plus a small handful of
   musl-required syscalls (TLS, time, sigmask stubs, writev, getrandom).

The gate for declaring v0.0.3 complete is unambiguous: a static
`hello_world.c` compiled by stock clang against musl, dropped into the
MINIX FS image, runs from boot and prints to the framebuffer + serial.

---

## 2. Locked decisions

These were debated and resolved for v0.0.3. They are not open for
casual revision during implementation.

### 2.1 Release thesis

v0.0.3 is the **"real userspace runs" release**.

The release is complete when JNU can:

1. Boot, mount root, and `execve` a `/init` that is a statically-linked
   musl binary built by stock clang with no JNU-specific patches.
2. Resolve every syscall that musl's static startup path issues (see
   §2.7) without panicking and without `-ENOSYS` on a syscall that
   musl treats as fatal.
3. Have that binary call `printf("hello, jnu\n")` and `exit(0)`
   normally. The kernel reaps the child cleanly and idles.
4. Pass the v0.0.2.2 selftests unchanged, plus the new selftests
   listed per phase below.
5. Produce no leaks across 1000 fork/mmap/munmap/exit cycles
   (asserted by `mmap_selftest`).

### 2.2 Syscall ABI: Linux-compatible numbering

**`jnuspec2.md` §2.4 is superseded.** JNU's native syscall ABI adopts
**Linux x86_64 syscall numbers** for every syscall present in both.
The implementation remains from scratch; this changes integers, not
code or copied logic.

Rationale: the integers are not the ABI. The contract — register
layout, semantics, error handling — is. Picking the same numbers
Linux picked is no more "adopting Linux's ABI" than picking ELF as a
binary format. The §2.4 ban was a category error; this paragraph
fixes it.

The renumbered table:

```
read              0     (was JNU 2)
write             1     (was JNU 1)            ✓ unchanged
open              2     (was JNU 3)
close             3     (was JNU 4)
fstat             5     (was JNU 8)
lseek             8     (was JNU 5)
mmap              9     NEW (slot was retired JNU spawn)
mprotect         10     NEW
munmap           11     NEW
rt_sigaction     13     NEW (stub, see §2.7)
rt_sigprocmask   14     NEW (stub, see §2.7)
ioctl            16     NEW (stub, see §2.7)
writev           20     NEW
sched_yield      24     (was JNU 7 "yield")
nanosleep        35     NEW
getpid           39     (was JNU 6)
fork             57     (was JNU 11)
execve           59     (was JNU 12)
exit             60     (was JNU 0)
wait4            61     (was JNU 10 "waitpid"; libjnu wraps as waitpid)
arch_prctl      158     NEW
set_tid_address 218     NEW (stub, see §2.7)
clock_gettime   228     NEW
exit_group      231     NEW (alias of exit; JNU has no threads yet)
getrandom       318     NEW
```

JNU-private syscalls — calls with no Linux equivalent or with
deliberately divergent semantics — get numbers in `[1024, 1535]`. The
range is reserved at the spec level: nothing else may take it. v0.0.3
introduces no JNU-private syscalls; the range is held for future use.

`spawn` (retired, was JNU 9) is dropped completely. The slot now
belongs to `mmap`. There is no compatibility shim.

The renumber is mechanical and atomic: one diff to
`kernel/include/jnu/syscall_nr.h`, one matching diff to
`user/libjnu/include/jnu_syscall.h`, one update to the dispatch
table. After this diff lands, every existing user binary in the test
image must be rebuilt. No grace period; there are no production
binaries.

### 2.3 `mmap` policy

Signature, exactly Linux-compatible:

```c
void *mmap(void *addr, size_t length, int prot, int flags,
           int fd, off_t offset);
```

Flag values (Linux-compatible):

```
PROT_NONE       0x00
PROT_READ       0x01
PROT_WRITE      0x02
PROT_EXEC       0x04

MAP_PRIVATE     0x02
MAP_FIXED       0x10
MAP_ANONYMOUS   0x20
```

v0.0.3 supports **only `MAP_PRIVATE | MAP_ANONYMOUS`**. Calls with
any other flag combination return `-EINVAL`. File-backed mappings
and `MAP_SHARED` land in v0.0.4 alongside the page cache.

Concrete contract:

- `length` is rounded up to a page; `length == 0` ⇒ `-EINVAL`;
  `addr + length` overflowing user space ⇒ `-EINVAL`.
- `fd` must be `-1` (anonymous), `offset` must be `0`. Otherwise
  `-EINVAL`.
- W^X is enforced at the syscall boundary: `(prot & PROT_WRITE) &&
  (prot & PROT_EXEC)` ⇒ `-EINVAL`.
- Without `MAP_FIXED`, `addr` is a hint. `NULL` ⇒ kernel picks. The
  picker walks top-down from `MMAP_BASE = 0x0000_5555_0000_0000`,
  finding the first VMA-tree gap of at least `length` page-aligned
  bytes.
- With `MAP_FIXED`, `addr` must be page-aligned and entirely within
  user space. Existing mappings in `[addr, addr+length)` are
  *replaced* (Linux semantics): every overlapping VMA is split or
  removed and its PTEs are torn down before the new VMA is
  installed. This is the dynamic-loader-friendly behavior.
- The VMA is created **without installing PTEs**. First access to
  any page in the VMA faults; the `#PF` handler resolves it (see
  §2.5).
- Returned address is page-aligned. On failure, the syscall returns
  the negated errno via `rax` per §5.2 of `jnuspec2.md` (i.e.
  `(uint64_t)(-EINVAL)`), not `MAP_FAILED`. libjnu's `mmap` wrapper
  performs the conversion to the C-friendly `MAP_FAILED` convention.

### 2.4 `munmap` and `mprotect` policy

Signatures:

```c
int munmap(void *addr, size_t length);
int mprotect(void *addr, size_t length, int prot);
```

- Both require page-aligned `addr`. `length` is rounded up to a
  page. Both walk the VMA tree and **split VMAs as needed** when
  the range cuts a VMA partially.
- `munmap` of a hole (no VMA covers part of the range) succeeds for
  the covered parts — Linux semantics.
- `mprotect` rejects `PROT_WRITE | PROT_EXEC` (`-EINVAL`).
- `mprotect` updates VMA flags first, then walks present PTEs to
  apply the new permissions.
- **CoW interaction**: if `mprotect` adds `PROT_WRITE` to a VMA that
  contains CoW-shared pages (refcount > 1 *or* the zero page),
  `PTE_WRITE` is **not** set on those PTEs. The VMA records the new
  logical writability; the existing CoW fault path resolves the
  share on first write. No new code in `mprotect` for this — the
  `vmm_handle_cow_fault` path from v0.0.2.2 is the single resolver.
- `mprotect` removing `PROT_WRITE` clears `PTE_WRITE` on all present
  PTEs and `invlpg`s each.
- `mprotect` adding/removing `PROT_EXEC` flips `PTE_NX` on present
  PTEs accordingly.

VMA splitting is the highest-risk diff in this release. It gets a
dedicated selftest (`vma_split_selftest`, §5).

### 2.5 Lazy zero-fill and zero-page CoW

The PMM gains a single global, **the kernel zero page**:

- Allocated once at PMM init via `pmm_alloc_zeroed_pages(0)`,
  exposed as `paddr_t mm_zero_page;`.
- Its PFN's refcount stays `0` forever. It is **never** passed to
  `pmm_get_user_page` or `pmm_put_user_page`. Both functions panic
  on the zero page (defense-in-depth assertion).
- It is never freed.

Lazy zero-fill rules in the user `#PF` handler, when the fault is on
a VMA covered by the address-space VMA tree and the PTE is absent:

1. **Read fault** with `(vma->flags & VMA_READ)`: install a PTE
   pointing at `mm_zero_page` with `PTE_USER | PTE_NX`,
   `PTE_WRITE` clear. No allocation, no refcount touch.
2. **Write fault** with `(vma->flags & VMA_WRITE)`: allocate a
   fresh user page via `pmm_alloc_user_page()` (already zero —
   §2.6 makes the freelist zeroed). Install with
   `PTE_USER | PTE_WRITE | PTE_NX`. No refcount get; the alloc
   set it to 1.
3. **Instruction-fetch fault** with `(vma->flags & VMA_EXEC)`:
   allocate a fresh user page (already zero), install with
   `PTE_USER` and `PTE_NX` clear, `PTE_WRITE` clear. No refcount
   get.

If none of the above applies (e.g. write fault on a non-`VMA_WRITE`
VMA, or no VMA covers the address), the fault falls through to the
existing "kill the user process" path.

The CoW handler from v0.0.2.2 needs **one** new check: any operation
that decrements the refcount of `old_pa` is skipped if
`old_pa == mm_zero_page`. The same check is added in
`paging_unmap` and `paging_destroy_user_half`. Three sites, one
branch each.

The zero-page-CoW write fault flows naturally through the existing
slow path: refcount comparison says "not 1" (it's 0), the slow path
allocates a fresh page, `memcpy`'s the zero page (a no-op-ish copy
of zeros), installs the new PTE, *skips* the `pmm_put_user_page` for
the old PA because it's the zero page. Same single resolver, same
single fault path.

PROT_EXEC anon mappings do not benefit from the zero-page
optimization: their first instruction fetch always materializes a
real page. Acceptable; PROT_EXEC anon-mmaps are rare in practice.

### 2.6 Page zero-on-free

`pmm_free_pages(pa, order)` zeroes `(1 << order) * PAGE_SIZE` bytes
through the HHDM **before** returning the block to the buddy free
list. Zero is the new freelist invariant.

Consequences:

- `pmm_alloc_zeroed_pages` becomes a thin wrapper around
  `pmm_alloc_pages`; the redundant `memset` is removed.
- `pmm_alloc_user_page` likewise drops its zero step (the alloc-zeroed
  path).
- Information-disclosure risk from §12 of `jnuspec.md` is closed.
- Slab caches keep their existing per-object reuse semantics; this
  change applies at the buddy boundary only. Slab object zero-on-free
  is a separate question, deferred.

Cost: ~600 ns/page on modern silicon, dominated by memory bandwidth.
A 4 MiB free in a single `pmm_free_pages(... order=10)` call costs
roughly 600 µs once. Acceptable.

### 2.7 FPU/SSE state save

Eager save/restore on every context switch. No lazy-FPU CR0.TS
games. `#NM` (vector 7) panics — its appearance is a setup bug.

Detection at boot:

- `CPUID.01H:ECX.XSAVE` (bit 26) → use `XSAVE`/`XRSTOR`.
- Otherwise → use `FXSAVE`/`FXRSTOR`.
- Either path requires `CPUID.01H:EDX.FXSR` (bit 24); absence ⇒
  `panic("cpu lacks FXSR")`.

Boot-time CR0/CR4 setup (in `cpu_init`):

```
CR0.MP   = 1
CR0.EM   = 0
CR0.NE   = 1
CR0.TS   = 0   /* stays 0 forever */
CR4.OSFXSR     = 1
CR4.OSXMMEXCPT = 1
CR4.OSXSAVE    = 1   /* if XSAVE supported */
```

If `XSAVE` is supported:

- Set `XCR0 = X87 | SSE = 0x3`. **No AVX, no AVX-512 in v0.0.3.**
- Query `CPUID.0DH:EBX` *after* setting XCR0 to obtain the required
  state-area size for the enabled feature set. Cache it as
  `cpu_xsave_size`.

Otherwise (legacy FXSAVE path):

- `cpu_xsave_size = 512` (FXSAVE area).

Per-task buffer:

- Embedded in `struct task` with `_Alignas(64)`, sized at `cpu_xsave_size`.
- Allocated with the task, freed with it. No separate refcount.
- New tasks copy from a global `fpu_init_state` buffer in `.rodata`
  (canonical FNINIT-equivalent with `MXCSR = 0x1F80`). This avoids a
  per-task `fninit + ldmxcsr` dance and is deterministic.

Context-switch hook (in `sched`):

- On switch-out: `XSAVE` (or `FXSAVE`) into the outgoing task's
  buffer.
- On switch-in: `XRSTOR` (or `FXRSTOR`) from the incoming task's
  buffer.

The kernel itself remains SSE-free. `-mgeneral-regs-only` continues
to apply to all kernel C and assembly. No floats in kernel code; no
SSE in `memcpy` and friends.

Compile-time assertion: `_Static_assert(sizeof(struct task) % 64 == 0)`
is not required, but the FPU buffer field within `struct task` must
be aligned by `_Alignas(64)` and the task allocator must hand out
64-byte-aligned `struct task` instances.

### 2.8 `execve` refactor

Goal: exactly one code path creates user mappings.

A new helper:

```c
int vmm_map_anonymous(struct addr_space *space, vaddr_t addr,
                      size_t length, uint32_t prot, uint32_t flags);
```

- `prot` and `flags` use the user-facing `PROT_*` / `MAP_*` values.
- `flags` must include `MAP_PRIVATE | MAP_ANONYMOUS`. With
  `MAP_FIXED`, `addr` is required and existing overlaps are
  replaced as in §2.3.
- Returns 0 on success, negative errno on failure. Allocates and
  inserts the VMA; does *not* install PTEs.

The `mmap` syscall is a direct wrapper around `vmm_map_anonymous`.

`execve` is refactored as follows:

1. Build a fresh `addr_space` via `vmm_create_space()`.
2. For each `PT_LOAD` segment of the new ELF:
   `vmm_map_anonymous(new_space, seg_va, seg_len,
                      seg_prot, MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS)`.
3. Copy file contents into the segment via the HHDM (same primitive
   that the page-fault handler uses to materialize anon pages, but
   here we materialize eagerly to a kmalloc'd staging buffer first
   so the address space install is atomic).
4. Stack VMA: `vmm_map_anonymous(new_space, stack_top - stack_size,
                                 stack_size, PROT_READ|PROT_WRITE,
                                 MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS)`.
5. **Stack guard page**:
   `vmm_map_anonymous(new_space, stack_top - stack_size - PAGE_SIZE,
                      PAGE_SIZE, PROT_NONE,
                      MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS)`.
   Stack overflow now traps at the guard, killing the process with a
   clean `pagefault: write to non-readable page` rather than
   silently corrupting an adjacent VMA.
6. Atomically swap the new address space into the running process,
   tear down the old one (existing v0.0.2 logic, unchanged).

Lifecycle responsibility for staging buffers, fd close-on-exec, and
argv/envp copying is unchanged from v0.0.2.

### 2.9 musl support syscalls

A statically-linked musl userspace exercises a small set of syscalls
during startup that v0.0.2.2 does not provide. v0.0.3 adds exactly
these, in their minimum-viable form. The list is closed: anything not
in this list either uses an existing JNU syscall or is left
unimplemented (returning `-ENOSYS`, which musl handles).

| Linux # | Name             | v0.0.3 implementation                                                                                  |
| ------- | ---------------- | ------------------------------------------------------------------------------------------------------ |
|      13 | `rt_sigaction`   | **Stub.** Returns 0. Stores nothing. No signals are delivered in v0.0.3.                                |
|      14 | `rt_sigprocmask` | **Stub.** Returns 0. Stores nothing.                                                                    |
|      16 | `ioctl`          | **Stub.** Returns `-ENOTTY` for every request, regardless of fd. musl falls back gracefully.            |
|      20 | `writev`         | Real. Walks the iovec, calls the existing write path for each entry. Returns total bytes written.       |
|      35 | `nanosleep`      | Real. TSC-based busy-yield: `sched_yield()` in a loop until the deadline elapses. No high-res timers.   |
|     158 | `arch_prctl`     | Real. Handles `ARCH_SET_FS` / `ARCH_GET_FS` / `ARCH_SET_GS` / `ARCH_GET_GS` via the FSBASE/GSBASE MSRs. |
|     218 | `set_tid_address`| **Stub.** Returns the current task's pid (TID == PID; no threads).                                      |
|     228 | `clock_gettime`  | Real. `CLOCK_REALTIME` = boot RTC + TSC delta; `CLOCK_MONOTONIC` = TSC delta from boot. Other clock IDs return `-EINVAL`. |
|     231 | `exit_group`     | Alias of `exit` (60). Same handler.                                                                     |
|     318 | `getrandom`      | TSC-mixed PRNG (xorshift64 seeded at boot from TSC + RTC). **Documented as not cryptographic.** Real CSPRNG lands in v0.0.5+. |

`arch_prctl(ARCH_SET_FS, ...)` is load-bearing for musl: TLS access
through `%fs:0x...` reads `errno` and the thread-local storage block.
The implementation writes the value to `MSR_FS_BASE` for the current
task and stores it in the task struct so context switches preserve it
(via `wrmsr(MSR_FS_BASE, task->fs_base)` on switch-in).

`clock_gettime` correctness depends on TSC calibration, which already
exists in v0.0.2 via PIT calibration. The boot RTC read is also
existing infrastructure.

`getrandom` returning predictable bytes is a deliberate choice:
musl's stack-protector init wants entropy, and silently giving it
deterministic-but-distinct-per-boot bytes is better than refusing.
Real entropy lands when there's a real source.

The list is justified by *minimum musl static startup*; if a future
musl release adds a new mandatory syscall, that's a v0.0.3 follow-up
patch, not a new release.

### 2.10 Demo: static musl Hello World

The release ships an artifact under `user/demo/hello/`:

- `hello.c`: `#include <stdio.h>` + `printf("hello, jnu\n"); return 0;`
- A makefile fragment that:
  1. Builds `hello.c` with stock clang against a vendored musl, fully
     static, no dynamic linking.
  2. Drops the resulting `hello` binary into the test MINIX FS image
     at `/init`.
- The cmdline `init=/init` (parsed by the existing cmdline code from
  jnuspec2) selects this binary as PID 1.
- On boot with `init=/init`, the kernel boots, mounts the rootfs,
  execs `/init`, and `hello, jnu\n` appears on the framebuffer and
  the COM1 serial. The kernel reaps the child cleanly and idles.

This is the pass/fail gate of the release. Everything else exists to
support this single observable.

### 2.11 Non-goals

The following remain deferred and are **not** introduced by v0.0.3:

| Feature                              | Target        |
| ------------------------------------ | ------------- |
| File-backed `mmap` (page cache)      | v0.0.4        |
| `MAP_SHARED`                         | v0.0.4        |
| AVX / AVX-512 state                  | when needed   |
| `brk` / `sbrk`                       | never         |
| Real signals (delivery + handlers)   | v0.0.4 or 0.0.3.1 |
| `mremap` / `madvise` / `msync` / `mincore` | v0.0.5+ |
| `MAP_GROWSDOWN` / stack auto-grow    | v0.0.5+       |
| KASLR                                | v0.0.4+       |
| Cryptographic `getrandom`            | v0.0.5+       |
| virtio-blk / virtio-net              | v0.0.4        |
| Threads (`clone`, `futex`)           | v0.0.5+       |
| Linux compatibility translation layer| v0.0.4+       |

---

## 3. Coding style

v0.0.3 inherits coding style from `jnuspec022.md` §3, which inherits
from `jnuspec021.md` §3, which inherits from `jnuspec2.md` §3, which
inherits from `jnuspec.md` §3. No clarifications are added here.

The `goto fail_*` cleanup rule from `jnuspec2.md` §3.1 is mandatory
for every new resource-acquiring path introduced in this release. In
particular:

- `vmm_map_anonymous` must release any partially-inserted VMA on
  any post-allocation failure.
- The lazy zero-fill `#PF` handler must release the freshly-allocated
  page on `paging_map` failure.
- `execve`'s segment-loading loop must tear the new address space
  down via `vmm_destroy_space` on any segment-load failure, before
  returning to the caller.

VMA-splitting helpers follow `subsys_verb_noun` snake_case
(`vma_split_at`, `vma_remove_range`, `vma_find_gap_top_down`).

---

## 4. Phases

The release is implemented in four phases. Phases are sequential;
landing Phase N+1 before Phase N is a spec violation.

### 4.1 Phase 1 — Substrate

**Objective**: lay the substrate that the rest of the release builds
on, with no user-visible changes yet.

Deliverables:

- **Linux syscall renumber**, atomic patch:
  - `kernel/include/jnu/syscall_nr.h` rewritten per §2.2.
  - `user/libjnu/include/jnu_syscall.h` rewritten to match.
  - The dispatch table in `kernel/syscall/dispatch.c` becomes a
    sparse array indexed by syscall number, sized to the highest
    number in use plus one. Every existing syscall handler is
    re-wired to its new number; semantics unchanged.
  - libjnu's `waitpid()` C wrapper now translates to `wait4(pid,
    status, 0, NULL)` and discards `rusage`.
  - libjnu's `yield()` C wrapper now translates to `sched_yield()`.
  - All existing user binaries in the test image are rebuilt.
- **Page zero-on-free** in `kernel/mm/pmm.c`:
  - `pmm_free_pages` gains a `memset(phys_to_virt(pa), 0,
    PMM_ORDER_SIZE(order))` before the buddy push.
  - `pmm_alloc_zeroed_pages` becomes a one-liner forwarding to
    `pmm_alloc_pages`.
  - `pmm_alloc_user_page` drops its zero step.
- **FPU/SSE eager save** per §2.7:
  - `cpu_init` in `kernel/arch/x86_64/cpu.c` sets the CR0/CR4 bits
    and configures XCR0 if XSAVE is available; caches `cpu_xsave_size`.
  - `struct task` gains `_Alignas(64) uint8_t fpu_state[]` (or a
    pointer to a 64-byte-aligned allocation; the choice is local to
    the implementer, but the *task allocator* must guarantee
    alignment).
  - `kernel/arch/x86_64/context.S` adds inline `XSAVE`/`XRSTOR`
    (or `FXSAVE`/`FXRSTOR`) at the documented save/restore points.
  - A static `fpu_init_state[512]` (or sized to `cpu_xsave_size`) in
    `.rodata` provides the canonical starting state, copied into
    each new task's buffer at task creation.
  - `#NM` (vector 7) handler is changed from "kill user" to
    `panic("FPU #NM with eager save active")`.

Phase 1 success criteria:

- All v0.0.2.2 selftests pass with the renumbered ABI and the new
  PMM/FPU substrate.
- `pmm_zerofree_selftest`: write a sentinel pattern to a page, free
  it, allocate it back, assert the sentinel is gone.
- `fpu_selftest`: spawn two kernel-mode tasks; each loads distinct
  XMM/x87 patterns; yield between them; assert each task sees its
  own pattern after the round-trip.

### 4.2 Phase 2 — `mmap` / `munmap` / `mprotect`

**Objective**: ship the three mapping syscalls and the lazy-fill +
zero-page-CoW fault path.

Deliverables:

- `kernel/mm/vma.c` grows VMA splitting:
  - `vma_split_at(vma, vaddr_t boundary)` — split a VMA at a page
    boundary, returning two adjacent VMAs.
  - `vma_remove_range(tree, vaddr_t start, vaddr_t end)` — remove
    every VMA fully inside the range; split the partial-overlap
    boundary VMAs.
  - `vma_find_gap_top_down(tree, size)` — top-down first-fit gap
    finder used by `mmap` without `MAP_FIXED`.
- `kernel/mm/mmap.c` (new file):
  - `vmm_map_anonymous` per §2.8.
  - `sys_mmap`, `sys_munmap`, `sys_mprotect` thin syscall wrappers.
- `kernel/mm/pmm.c` gains `mm_zero_page` initialization in
  `pmm_init`, plus the `pmm_get_user_page` / `pmm_put_user_page`
  panic-on-zero-page guards.
- `kernel/arch/x86_64/exceptions.c` `#PF` handler grows the lazy
  zero-fill cases per §2.5.
- `kernel/mm/vmm.c` `vmm_handle_cow_fault` gets the
  `if (old_pa != mm_zero_page)` guard around `pmm_put_user_page`.
- `kernel/arch/x86_64/paging.c` `paging_unmap` and
  `paging_destroy_user_half` get the same `mm_zero_page` skip
  before any `pmm_put_user_page`.
- Syscall dispatch table entries for 9, 10, 11.

Phase 2 success criteria:

- `vma_split_selftest`: 256 randomized split/remove/insert
  operations against a model VMA tree, asserting equality with a
  sorted-list reference.
- `mmap_selftest`: round-trip allocate / write / munmap with both
  hint and `MAP_FIXED`, including a `MAP_FIXED` over an existing
  mapping and verifying the existing mapping was replaced.
- `mmap_lazy_selftest`: `mmap` 64 MiB, read first byte (assert 0),
  measure PMM frame count delta is exactly 0 (zero-page wired in,
  no real frame allocated). Write first byte, measure delta is
  exactly 1.
- `mmap_cow_selftest`: `mmap` writable region, write a sentinel,
  fork, child writes a different sentinel, assert each side sees
  its own value, refcount ladder verified.
- `mprotect_wx_selftest`: `mprotect(..., PROT_WRITE|PROT_EXEC)`
  returns `-EINVAL` (and same for `mmap`).
- `mprotect_split_selftest`: `mprotect` middle of a VMA, assert
  the VMA was split into 3 with correct flags.

### 4.3 Phase 3 — `execve` refactor + musl-support syscalls

**Objective**: bring `execve` onto the unified mapping path, add the
guard page, and add the musl-required syscalls.

Deliverables:

- `kernel/kernel/execve.c` refactored per §2.8:
  - PT_LOAD loop now calls `vmm_map_anonymous` with
    `MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS` instead of its private
    mapping helper.
  - Stack VMA created via `vmm_map_anonymous`.
  - Guard page created via `vmm_map_anonymous(... PROT_NONE ...)`.
  - The private mapping helper that previously existed in
    `execve.c` is **deleted**, not kept around. Drift will reappear
    if it lingers.
- `kernel/syscall/sys_writev.c` (new) — real `writev`.
- `kernel/syscall/sys_arch_prctl.c` (new) — FS/GS base get/set.
- `kernel/syscall/sys_clock_gettime.c` (new) — TSC + RTC clocks.
- `kernel/syscall/sys_nanosleep.c` (new) — TSC-yield sleep.
- `kernel/syscall/sys_getrandom.c` (new) — xorshift64 PRNG.
- `kernel/syscall/sys_set_tid_address.c` (new, stub).
- `kernel/syscall/sys_rt_sigaction.c` (new, stub).
- `kernel/syscall/sys_rt_sigprocmask.c` (new, stub).
- `kernel/syscall/sys_ioctl.c` (new, stub returning `-ENOTTY`).
- `kernel/syscall/sys_exit_group.c` (new) — calls the same handler
  as `sys_exit`.
- `struct task` gains `uint64_t fs_base;`. Switch-in writes it via
  `wrmsr(MSR_FS_BASE, task->fs_base)`.

Phase 3 success criteria:

- All v0.0.2.2 + Phase 1 + Phase 2 selftests still pass.
- `execve_guard_selftest`: a deliberately-crafted user program
  underflows its stack and is killed by the guard page; the
  kernel does not panic and the next process boots normally.
- `arch_prctl_selftest`: a tiny user program calls
  `arch_prctl(ARCH_SET_FS, &val)`, then reads through `%fs:0`,
  asserts it sees `val`. Two such programs run concurrently and
  see distinct values across yields.
- `clock_gettime_selftest`: two consecutive `CLOCK_MONOTONIC` reads
  differ by a positive amount; `CLOCK_REALTIME` is a plausible
  Unix epoch second value.

### 4.4 Phase 4 — Demo

**Objective**: prove "real userspace runs" with an unmodified
toolchain.

Deliverables:

- `user/demo/hello/hello.c` and `user/demo/hello/Makefile`:
  - Vendored musl source under `user/musl/` (a pinned musl tag,
    built once, no JNU patches).
  - `hello.c` calls `printf` and `exit`. No JNU-specific
    extensions, no inline syscalls.
  - The makefile produces a fully-static `hello` binary using
    stock clang and the vendored musl. The binary is ~100 KiB.
- `scripts/mkimage.sh` (existing) is updated to drop the binary
  into the test MINIX FS at `/init`.
- `kernel/kernel/main.c` honors `init=/init` from cmdline (parser
  exists from jnuspec2). With `init=/init`, the kernel `execve`s
  this binary as PID 1 instead of running the v0.0.2 built-in test
  init.

Phase 4 success criteria — these are the v0.0.3 exit criteria:

JNU v0.0.3 is **complete** when, booted via Limine in QEMU with a
MINIX FS disk image and `init=/init`:

1. The kernel boots, mounts root, and `execve`s `/init`.
2. The static musl binary's `printf("hello, jnu\n")` writes "hello,
   jnu\n" to the framebuffer and to COM1 serial.
3. The binary's `return 0` from `main` flows through musl's
   `_exit_group` and lands in JNU's `sys_exit_group`.
4. The kernel reaps the child, prints `init exited`, and idles.
5. With `selftest=1`, every selftest from v0.0.1 through Phase 3
   passes within 5 seconds of boot, *and* the demo still runs
   afterwards.
6. Identical behavior on:
   - `qemu-system-x86_64 -machine q35 -m 256M`
   - `qemu-system-x86_64 -machine q35 -m 256M -enable-kvm`
7. No leaks across 1000 iterations of a `fork → mmap → munmap →
   exec(/init) → exit` stress harness, asserted by the existing
   PMM leak-detection selftest extended to count VMAs.

When all seven are green, tag `v0.0.3`.

---

## 5. Selftest budget summary

| Selftest                      | Phase | What it proves                                          |
| ----------------------------- | ----- | ------------------------------------------------------- |
| `pmm_zerofree_selftest`       | 1     | Freed pages are zero on next alloc.                     |
| `fpu_selftest`                | 1     | FPU state is per-task and survives context switches.    |
| `vma_split_selftest`          | 2     | VMA tree splits/coalesces match a reference model.      |
| `mmap_selftest`               | 2     | `mmap` round-trip with hint and `MAP_FIXED`.            |
| `mmap_lazy_selftest`          | 2     | Read fault wires zero page; only writes allocate.       |
| `mmap_cow_selftest`           | 2     | mmap'd region forks with correct CoW + refcount.        |
| `mprotect_wx_selftest`        | 2     | W^X enforced at both `mmap` and `mprotect`.             |
| `mprotect_split_selftest`     | 2     | `mprotect` of a sub-range splits the surrounding VMA.   |
| `execve_guard_selftest`       | 3     | Stack underflow hits the guard page, not other VMAs.    |
| `arch_prctl_selftest`         | 3     | `%fs:0` access works per-task across yields.            |
| `clock_gettime_selftest`      | 3     | Monotonic + realtime clocks return plausible values.    |
| `hello_demo`                  | 4     | Static musl `/init` prints to console and exits clean.  |

---

## 6. Risks and mitigations

| Risk                                                         | Mitigation                                                                               |
| ------------------------------------------------------------ | ---------------------------------------------------------------------------------------- |
| VMA-splitting bugs (highest-risk diff)                       | Dedicated `vma_split_selftest` with 256 randomized operations vs reference model.        |
| `MAP_FIXED` replace-on-collision corrupts adjacent VMAs      | `vma_remove_range` + selftest that asserts neighboring VMAs are intact after replacement.|
| Lazy zero-fill races with `mprotect` removing the VMA        | Single-CPU + IRQs off in fault path; spec §2.4 of `jnuspec022.md` already constrains this.|
| Zero-page accidentally refcounted                            | `pmm_get_user_page` / `pmm_put_user_page` panic on `mm_zero_page` (defense in depth).    |
| FPU state leak across tasks                                  | Eager save/restore; `#NM` panics; `fpu_selftest` checks isolation.                       |
| `arch_prctl(ARCH_SET_FS)` not preserved across switches      | `task->fs_base` on switch-in `wrmsr`; `arch_prctl_selftest` two-task assertion.          |
| musl static binary drifts and breaks the demo                | Vendor musl at a pinned tag; rebuild reproducibly from source in CI.                     |
| Linux number renumber misses a libjnu callsite               | Phase 1 renumber is atomic; full rebuild required; CI fails on any unresolved symbol.    |
| `clock_gettime` returns nonsense before TSC calibrated       | TSC calibration already exists in v0.0.2 (PIT); selftest asserts plausible values.       |
| `getrandom` predictability is mistaken for a CSPRNG          | Documented in syscall handler header comment; spec §2.9 calls it "not cryptographic".    |
| `execve` private mapping helper survives the refactor        | Phase 3 deliverable explicitly **deletes** the old helper.                               |

---

## 7. Document discipline

- Do not add a syscall in this release that is not in §2.9 unless
  musl proves to need it. If you add one, update §2.9 in the same
  patch series as the code.
- Do not add a flag to `mmap` / `mprotect` / `munmap` beyond §2.3.
  `MAP_SHARED`, file-backed `mmap`, and `madvise` belong to v0.0.4+.
- Do not enable AVX or AVX-512 in XCR0. v0.0.3 is x87 + SSE only.
- Do not implement signals beyond the §2.9 stubs.
- Do not implement `brk`. Ever.
- Do not retain the `execve` private mapping helper after Phase 3.
- The Linux-compatibility translation layer mentioned in
  `jnuspec2.md` line 75 is still v0.0.4+ work; v0.0.3 only adopts
  Linux's *integers* for native syscalls, not Linux's full ABI.
- If reality changes, update this document **and** the parent specs
  it supersedes (`jnuspec2.md` §2.4) in the same patch series as the
  code.
