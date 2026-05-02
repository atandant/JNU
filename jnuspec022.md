# JNU Specification — v0.0.2.2

> **JNU** — *J is not Unix.* A monolithic, x86_64, freestanding kernel
> written from scratch in GNU C17 + Intel-syntax NASM, booted by Limine,
> licensed GPLv2.
>
> This document is the contract for v0.0.2.2 and the master prompt for
> its implementation. v0.0.2.2 is the **copy-on-write release**. It
> introduces PMM page refcounts and flips `fork` from full-copy to CoW.
> Nothing else.
>
> v0.0.2.2 is a **delta release**. It inherits everything from
> `jnuspec021.md` (which inherits from `jnuspec2.md`, which inherits
> from `jnuspec.md`) unless this document explicitly overrides it. If
> something is not mentioned here, jnuspec021.md still applies.
>
> Do not silently deviate. If this document disagrees with reality on
> the ground, fix the spec or fix the code, never neither.

---

## 1. Identity and scope

| Property         | Value                                              |
| ---------------- | -------------------------------------------------- |
| Name             | JNU (J is not Unix)                                |
| Version          | 0.0.2.2                                            |
| Parent spec      | `jnuspec021.md` (chains to `jnuspec2.md`/`jnuspec.md`) |
| Architecture     | x86_64 only                                        |
| Process model    | `fork` + `execve`, **CoW**                         |
| License          | GPLv2                                              |

Everything not listed here is **unchanged from jnuspec021.md**. Repo
layout, boot flow, build system, scheduler policy, security baseline,
ELF policy, syscall ABI, fd table policy, argv/envp policy, and the
`fork`/`execve` user-visible semantics are all unchanged.

The release ships exactly two features: **PMM page refcounts** and
**copy-on-write fork**. There are no syscall additions, no ABI
changes, no driver changes, no FS changes. If a patch in this release
touches anything outside `kernel/mm/`, `kernel/arch/x86_64/paging.c`,
the `#PF` handler in `kernel/arch/x86_64/exceptions.c`, and
`kernel/kernel/fork.c`, it does not belong in v0.0.2.2.

---

## 2. Locked decisions

These were debated and resolved for v0.0.2.2. They are not open for
casual revision during implementation.

### 2.1 Release thesis

v0.0.2.2 is the **copy-on-write release**.

The release is complete when JNU can:

1. Run `fork()` and observe that the parent's and child's user pages
   share physical frames at fork time, with no per-page copy on the
   fork path.
2. Take a write fault in either the parent or the child on a shared
   frame and resolve it transparently into a per-process private copy
   (the writer gets a fresh frame, the other side keeps the original).
3. Take a write fault on a frame whose refcount has dropped to 1 and
   resolve it by simply re-enabling `PTE_WRITE`, with no allocation
   and no copy.
4. Pass repeated fork/exec/wait stress without leaking pages, frames,
   or refcounts.
5. Survive a `fork`-then-immediate-`exec` workload without copying any
   page that the child never reads or writes.

The user-visible semantics of `fork` from `jnuspec021.md` §2.4 / §2.8
are preserved exactly. CoW is an MM-internal optimization; userspace
must not be able to observe whether a fault was a CoW resolution or a
genuine access violation, beyond a possible difference in the timing
of `RSS`-style accounting that JNU does not yet expose.

### 2.2 PMM page refcounts

- Every 4 KiB user page tracked by the buddy allocator gains a
  16-bit refcount, stored alongside the existing `struct pmm_pfn`
  metadata. Adding a `uint16_t refcount` field is the entire data
  structure change; no separate refcount array, no per-page lock.
- The refcount is **only** maintained for pages allocated through a
  new `pmm_alloc_user_page()` path or its `_zeroed` variant — kernel
  page-table pages, slab pages, and large kmalloc allocations are
  not refcounted (their lifetime is governed by their owner). The
  existing `pmm_alloc_user_page()` symbol in `pmm.h` is repurposed
  to set refcount = 1 on allocation; current callers do not need to
  change.
- New helpers, in `kernel/mm/pmm.c`:
  - `void pmm_get_user_page(paddr_t pa);` — increments refcount.
  - `void pmm_put_user_page(paddr_t pa);` — decrements; frees the
    page back to the buddy allocator when the count reaches 0.
- `pmm_free_pages()` remains the unrefcounted free path used by
  kernel-internal allocations and by `pmm_put_user_page` after the
  refcount hits 0. Callers that previously freed user pages via
  `pmm_free_pages()` switch to `pmm_put_user_page()` exactly when
  the page they are freeing was allocated via
  `pmm_alloc_user_page()`. Mismatching the pair is a kernel bug.
- Refcount overflow (a page shared by 2^16 mappings) panics. This
  is unreachable in practice and the panic is cheaper than a
  saturating arithmetic check on every fork.
- A `pmm_user_refcount(pa)` accessor is exposed for selftest use
  only. It is not part of the kernel API.

### 2.3 CoW fork policy

- `vmm_clone_space()` (existing in `kernel/mm/clone_space.c`) flips
  from full-copy to CoW. For each present 4 KiB user PTE in the
  source space:
  1. Strip `PTE_WRITE` from the source PTE.
  2. Install the same physical address in the destination PTE,
     with `PTE_WRITE` cleared and the same `PTE_USER`/`PTE_NX`
     bits the source carries.
  3. Call `pmm_get_user_page(pa)` once per added reference.
  4. Invalidate the TLB for the source virtual address (`invlpg`)
     so the source side starts taking write faults too.
- The destination VMA still records the **logical** writability
  (the `VMA_WRITE` flag is preserved). The PTE is the source of
  truth for "is this page currently CoW-shared"; the VMA is the
  source of truth for "may this address ever be written".
- Pages that were already RO in the source (PT_LOAD without
  `PF_W`, e.g. `.text`) are shared without any PTE change beyond
  the refcount bump.
- `paging_destroy_user_half()` switches from `pmm_free_pages()` to
  `pmm_put_user_page()` for every user PTE it walks. Page-table
  pages remain freed via `pmm_free_pages()` — they are never
  refcounted.

### 2.4 Write-fault resolution

A new `vmm_handle_cow_fault(struct addr_space *space, vaddr_t va)`
sits in `kernel/mm/vmm.c` and is called from
`kernel/arch/x86_64/exceptions.c` when **all** of the following hold:

1. The fault is a `#PF` from user mode (`fault_from_user(st)`).
2. The error code has `PF_EC_W` set (write).
3. The error code has `PF_EC_P` set (page is present).
4. `vma_find()` returns a VMA that is `VMA_WRITE`.

If any condition fails, the fault falls through to the existing
"kill the user process" path — CoW does not relax any other
protection.

The handler:

1. Looks up the source PTE via `paging_get_flags()`.
2. If `pmm_user_refcount(pa) == 1`, OR `PTE_WRITE` back into the
   PTE, `invlpg(va)`, return resolved.
3. Otherwise allocate a fresh page via `pmm_alloc_user_page()`,
   `memcpy` the old page through the HHDM, install the new PTE
   with `PTE_WRITE` set, `pmm_put_user_page()` on the old page,
   `invlpg(va)`, return resolved.

The handler runs with IRQs in whatever state the trap entered with
(typically off for kernel-mode-from-user) and takes no locks.
Single-CPU + preempt-disabled while in the IDT path keeps it sound.
SMP CoW is out of scope; the v0.0.5+ SMP work will revisit.

### 2.5 Non-goals

The following remain deferred per jnuspec021.md §2.2 and are **not**
introduced by v0.0.2.2:

| Feature                          | Target        |
| -------------------------------- | ------------- |
| `MAP_PRIVATE` / `MAP_SHARED`     | v0.0.3+       |
| `mmap`/`munmap` syscalls         | v0.0.3+       |
| Huge-page CoW (2 MiB)            | when needed   |
| TLB shootdown                    | v0.0.4 (SMP)  |
| Page replacement / swap          | v0.0.5+       |
| RSS / per-process page accounting| v0.0.5+       |
| `madvise(MADV_DONTNEED)`         | v0.0.5+       |
| Refcounting of kernel-only pages | never         |

---

## 3. Coding style

v0.0.2.2 inherits coding style from `jnuspec021.md` §3, which inherits
from `jnuspec2.md` §3, which inherits from `jnuspec.md` §3. No
clarifications are added here.

The `goto fail_*` cleanup rule (`jnuspec2.md` §3.1) is mandatory for
every new resource-acquiring path introduced in this release. In
particular `vmm_handle_cow_fault` must release the freshly allocated
page on any post-allocation failure path (e.g. paging_map failure).

The refcount API follows the `get`/`put` naming convention already
used by `file_get`/`file_put` in `kernel/user/fd.c`. Do not invent a
parallel naming scheme for pages.

---

## 4. Document discipline

- Do not add a syscall in this release.
- Do not change ABI struct layouts.
- Do not refcount kernel page-table pages, slab pages, or large
  kmalloc allocations.
- Do not introduce a per-page lock.
- Do not implement TLB shootdown.
- If reality changes, update this document **and** `jnuspec021.md` in
  the same patch series as the code.
