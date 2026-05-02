# JNU Specification - v0.0.2.1

> **JNU** - *J is not Unix.* A monolithic, x86_64, freestanding kernel
> written from scratch in GNU C17 + Intel-syntax NASM, booted by Limine,
> licensed GPLv2.
>
> This document is the contract for v0.0.2.1 and the master prompt for
> its implementation. v0.0.2.1 is the **fork/exec release**. It promotes
> JNU's process model from a single-shot `spawn` to real Unix-shaped
> `fork` and `execve`, freezes the syscall ABI, and pays the
> foundation tax that future Unix-style features (signals, shells,
> pipes-with-real-semantics) all sit on.
>
> v0.0.2.1 is a **delta release**. It inherits everything from
> `jnuspec2.md` (which inherits coding style from `jnuspec.md`) unless
> this document explicitly overrides it. If something is not mentioned
> here, jnuspec2.md still applies.
>
> Do not silently deviate. If this document disagrees with reality on
> the ground, fix the spec or fix the code, never neither.

---

## 1. Identity and scope

| Property         | Value                                      |
| ---------------- | ------------------------------------------ |
| Name             | JNU (J is not Unix)                        |
| Version          | 0.0.2.1                                    |
| Parent spec      | `jnuspec2.md` (which chains to `jnuspec.md`) |
| Architecture     | x86_64 only                                |
| Bootloader       | Limine                                     |
| Languages        | GNU C17, Intel-syntax NASM                 |
| Compiler         | clang (`--target=x86_64-unknown-none-elf`) |
| Linker           | lld                                        |
| Kernel design    | Monolithic, strict subsystem boundaries    |
| Userspace ABI    | Native JNU ABI, **frozen as of v0.0.2.1**  |
| Process model    | `fork` + `execve`, full-copy (no CoW)      |
| License          | GPLv2                                      |

Everything not listed here is **unchanged from jnuspec2.md**. In
particular, repo layout, boot flow, build system, scheduler policy,
security baseline, and ELF policy are unchanged.

---

## 2. Locked decisions

These were debated and resolved for v0.0.2.1. They are not open for
casual revision during implementation.

### 2.1 Release thesis

v0.0.2.1 is the **fork/exec release**.

The release is complete when JNU can:

1. Run `fork()` from userspace and observe a child with a duplicated
   address space, duplicated fd table, and `rax = 0` in the child.
2. Run `execve(path, argv, envp)` from userspace and observe the
   current process replaced by a fresh ELF64 image, with argv/envp
   visible to the new program's `main`.
3. Run `fork` followed by `execve` in the child and observe the
   parent receive the child's exit status through `waitpid`, exactly
   as the v0.0.2 `spawn` flow did.
4. Pass repeated fork/exec/wait stress without leaking pages,
   `struct file`, or kernel stacks.
5. Refuse malformed argv/envp (oversize, missing terminator, bad user
   pointer) safely, without kernel state corruption.

**Stage A only.** v0.0.2.1 ships **full-copy** fork. Every writable
*and* read-only user page is duplicated into the child's address space
at fork time. There is no copy-on-write, no shared physical frames
between parent and child, and no PMM-level refcount. CoW is the v0.0.2.2
release thesis. Do not smuggle CoW into v0.0.2.1.

### 2.2 Non-goals

The following are explicitly out of scope for v0.0.2.1 and remain
deferred per jnuspec2.md §2.2 unless this row overrides:

| Feature                         | Target        |
| ------------------------------- | ------------- |
| Copy-on-write fork              | v0.0.2.2      |
| PMM page refcounts              | v0.0.2.2      |
| `vfork`                         | never         |
| `CLOEXEC` fd flag               | v0.0.3+       |
| `MAP_PRIVATE` / `MAP_SHARED`    | v0.0.3+       |
| Signals on exec / on child exit | v0.0.3+       |
| `PATH` lookup, shell builtins   | userspace only, never kernel |
| envp interpreted by kernel      | never (kernel preserves, never reads) |
| Dynamic fd table (>32 slots)    | when needed   |
| MLFQ                            | jnuspec2.md §2.2 still applies |
| MINIX write                     | v0.0.3+ (moved here from v0.0.2.1+) |
| AHCI/SATA                       | v0.0.3+       |

The MINIX write deferral was previously listed as `v0.0.2.1+`. It is
moved out of this release. v0.0.2.1 is fork/exec only.

### 2.3 ABI stability

The native JNU syscall ABI is **frozen as of v0.0.2.1**.

- Numbers 0..10 (inherited from v0.0.2) keep their meanings forever.
- Number 9 (`JNU_SYS_spawn`) is **retired**. The dispatcher returns
  `-ENOSYS`. The number is reserved and shall **not** be reused for
  any future syscall.
- Numbers 11 (`fork`) and 12 (`execve`) are minted by this release and
  are stable from this release forward.
- `JNU_SYS_MAX` becomes 12.
- Future releases may **add** syscall numbers above the current max.
  They may not renumber, repurpose, or reuse existing or retired
  numbers.
- Flag bits passed to existing syscalls may grow; existing flag bits
  may not change meaning.
- Struct layouts crossing the user/kernel boundary (`struct jnu_stat`,
  argv/envp pointer arrays) are stable. Adding a field is a new
  syscall; widening an existing field is a new syscall.

A future Linux compatibility layer may translate Linux ABI calls into
native JNU calls, but native JNU remains the kernel's primary ABI and
its numbers do not move to suit Linux.

### 2.4 Process model policy

- v0.0.2.1 supports `fork` and `execve` only. There is no `vfork`,
  `clone`, `posix_spawn`, `forkexec`, or any other process creation
  primitive at the syscall layer.
- `fork` duplicates the calling process: address space (full copy),
  fd table (refcounted file objects), pid is fresh, parent linkage
  is set, kernel stack for the child is **forged**, not copied.
- `execve` replaces the calling process's address space and image
  with a fresh ELF64 load. pid is preserved. fd table is preserved
  (no CLOEXEC in v0.0.2.1). argv/envp are preserved across the
  teardown by trampolining through a kernel-owned buffer.
- `process_spawn` ceases to be a syscall. It remains as a
  **kernel-only** helper used for the initial bring-up of `/init`.
  It is not callable from userspace.
- `userspace must use fork+execve` is the contract. Internal kernel
  code and selftests may still use `process_spawn`.

### 2.5 Memory copy policy

- Fork copies every user-mapped page from the parent into the child.
  Read-only segments (e.g. ELF `.text`) are copied, not shared.
- The child's PML4 is fresh. Kernel-half mappings are installed by
  reusing the same construction path as `vmm_create_space`.
- The kernel never relies on physical-frame sharing in v0.0.2.1. No
  refcount field is added to the PMM. The only refcount introduced
  by this release is on `struct file`.
- Stage B (v0.0.2.2) will introduce `pmm_get_page`/`pmm_put_page` and
  flip fork to CoW. The refcount data structure for Stage B is
  explicitly out of scope here. Do not pre-bake it.

### 2.6 fd table policy

- `JNU_MAX_FDS` remains 32. The slot array stays statically sized.
- `struct file` gains a `refcount` field. `file_get`/`file_put`
  helpers are introduced. `fd_close` releases its slot reference;
  callers no longer free `struct file` directly.
- Fork increments the refcount of every non-NULL slot in the child's
  fd table. The `struct file` itself is shared. Offsets are shared.
  This is the standard Unix open-file-description contract.
- Execve does not close any fd in v0.0.2.1. CLOEXEC is deferred.
- Standard fds (0=stdin, 1=stdout, 2=stderr) survive both fork and
  execve unchanged.

### 2.7 argv/envp policy

- `execve` accepts `(const char *path, char *const argv[], char *const envp[])`.
- The combined size of the path string, argv pointer array, argv
  string contents, envp pointer array, and envp string contents,
  including all NUL terminators and the trailing NULL of each array,
  shall not exceed **64 KiB**.
- On overflow, `execve` returns `-E2BIG` and the calling process is
  unchanged.
- The kernel copies path, argv, and envp into a kernel-owned buffer
  **before** tearing down the old address space. The buffer is freed
  when the new image is set up or when execve fails.
- The kernel does **not** interpret envp. It preserves it across
  execve and exposes it to the new program's `main` via the standard
  `argc, argv, envp` shape on the user stack. PATH lookup, variable
  expansion, and inheritance semantics are userspace concerns.
- argv must have at least one entry (argv[0]). An empty argv returns
  `-EINVAL`.

### 2.8 fork return policy

- The parent's `fork` returns the child's pid (positive int).
- The child's `fork` returns 0.
- On any failure (no pid, no memory, fd table copy failure, address
  space clone failure), `fork` returns a negative errno in the parent
  and the child is not created.
- The child enters userspace at the same RIP/RSP as the parent's
  syscall return site, with the same registers except `rax = 0`.
  This is enforced by **forging** a syscall-return frame on the
  child's fresh kernel stack, not by copying the parent's stack.
- The child runs as a new task (`struct task`) in the scheduler. It is
  added to the runnable queue at fork time and may be preempted to
  before the parent returns.

### 2.9 Wait policy

- `waitpid` semantics are unchanged from v0.0.2 except that fork
  produces multiple children per parent. The existing parent/child
  linkage (`first_child`, `next_sibling`) and zombie reaping path
  remain authoritative. No new syscall is introduced.
- A parent waiting with `pid == -1` waits for any child. Existing
  v0.0.2 behavior covered single-child cases; v0.0.2.1 must handle
  arbitrary-many children correctly.
- A waitpid for a non-child pid returns `-ECHILD`.

---

## 3. Coding style

v0.0.2.1 inherits coding style from `jnuspec2.md` §3, which inherits
from `jnuspec.md`. No clarifications are added here.

The `goto fail_*` cleanup rule (`jnuspec2.md` §3.1) is mandatory for
every new resource-acquiring path introduced in this release. In
particular:

- `fork` (process alloc, addr space clone, fd table dup, kstack alloc,
  task alloc, scheduler insert).
- `execve` (argv/envp trampoline alloc, image read, addr space build,
  swap-and-destroy old, user stack setup).
- `vmm_clone_space` (per-VMA page alloc, page-table walk).
- argv/envp validation (per-pointer usercopy).

Any new function with more than one cleanup path that does not use the
canonical idiom is a review failure.

---

## 4. ABI additions

### 4.1 Syscall table delta

```c
#define JNU_SYS_exit    0
#define JNU_SYS_write   1
#define JNU_SYS_read    2
#define JNU_SYS_open    3
#define JNU_SYS_close   4
#define JNU_SYS_lseek   5
#define JNU_SYS_getpid  6
#define JNU_SYS_yield   7
#define JNU_SYS_fstat   8
/* 9: retired (was JNU_SYS_spawn). Reserved. Returns -ENOSYS. */
#define JNU_SYS_waitpid 10
#define JNU_SYS_fork    11
#define JNU_SYS_execve  12

#define JNU_SYS_MAX     12
```

### 4.2 `fork`

```
int fork(void);
```

- No arguments.
- Returns child pid in parent (positive), 0 in child, or `-errno` in
  parent on failure.
- Failure modes: `-ENOMEM`, `-EAGAIN` (pid table full).

### 4.3 `execve`

```
int execve(const char *path,
           char *const argv[],
           char *const envp[]);
```

- On success: does not return. The calling process's image is
  replaced.
- On failure: returns `-errno`, calling process is unchanged.
- Failure modes: `-EFAULT` (bad user pointer in path/argv/envp),
  `-E2BIG` (combined size > 64 KiB), `-ENOENT`, `-EACCES`, `-ENOEXEC`,
  `-ENOMEM`, `-EINVAL` (empty argv).
- On success the new program's `main` receives `(int argc, char
  **argv, char **envp)`. argv[argc] is NULL. envp is NULL-terminated.

### 4.4 Retired: `spawn`

`JNU_SYS_spawn` (9) is permanently retired. The dispatcher returns
`-ENOSYS` for this number. libjnu's `spawn()` wrapper is removed.
User programs that previously called `spawn(path, argv)` must use
`fork() + execve(path, argv, NULL)` directly or via a libjnu helper
of their own.

### 4.5 libjnu

- Add `fork()` wrapper (single syscall).
- Add `execve(path, argv, envp)` wrapper.
- Remove `spawn()` wrapper and `user/libjnu/spawn.c`.
- `crt0.S` already passes argv to `main`. Extend it to pass envp as
  the third argument. The user stack layout produced by the kernel
  must match.

---

## 5. Repository structure

Layout is unchanged from jnuspec2.md §4.

New files:

```
kernel/kernel/fork.c       /* fork() implementation */
kernel/kernel/execve.c     /* execve() implementation */
kernel/kernel/argv.c       /* argv/envp trampoline buffer */
kernel/syscall/sys_fork.c
kernel/syscall/sys_execve.c
kernel/mm/clone_space.c    /* vmm_clone_space() */
user/libjnu/fork.c
user/libjnu/execve.c
```

Removed files:

```
user/libjnu/spawn.c
```

Modified files (notable):

```
kernel/include/jnu/syscall_nr.h    /* +fork, +execve, retire 9    */
kernel/include/jnu/fd.h            /* + refcount on struct file   */
kernel/include/jnu/vmm.h           /* + vmm_clone_space prototype */
kernel/syscall/dispatch.c          /* + new entries, retire 9     */
user/libjnu/crt0.S                 /* + envp on user stack        */
user/init/main.c                   /* port spawn -> fork+execve    */
jnuspec2.md                        /* update non-goals table      */
```

The non-goals table update in `jnuspec2.md` is **mandatory in the
same patch series** that introduces v0.0.2.1's code. Per
jnuspec2.md §20.

---

## 6. Foundation pieces

### 6.1 `struct file` refcount

```c
struct file {
    enum jnu_file_type type;
    uint64_t           offset;
    uint32_t           flags;
    int                refcount;   /* new in v0.0.2.1 */
    union { ... }      u;
};

void file_get(struct file *f);
void file_put(struct file *f);   /* destroys at zero */
```

Rules:

- Newly allocated `struct file` starts at `refcount = 1`.
- `fd_alloc` does not bump refcount; the slot owns the initial ref.
- `fd_close` calls `file_put` on the removed slot.
- Fork's fd table dup loop calls `file_get` on every non-NULL slot.
- `file_destroy` becomes static to the fd subsystem and is reachable
  only via `file_put` reaching zero.
- Single-CPU only; refcount is plain `int`, mutated under preempt
  disable. SMP refcount semantics are deferred.

### 6.2 `vmm_clone_space`

```c
int vmm_clone_space(struct addr_space *src, struct addr_space **out);
```

- Allocates a fresh `addr_space` with a fresh PML4.
- Walks the source VMA tree in order. For each VMA, allocates new
  user pages, copies contents from the source's page tables, installs
  fresh PTEs in the destination with the same flags.
- Kernel-half mappings are installed identically to
  `vmm_create_space`.
- On any failure, unwinds all partial allocations. The destination
  must not leak frames or PML4 pages.
- W^X must hold on every cloned mapping. Writable+executable user
  mappings in the source are a kernel bug.

### 6.3 argv/envp trampoline

```c
struct exec_strings {
    char  *path;          /* kernel-owned copy of path */
    char **argv;          /* kernel-owned NULL-terminated array */
    char **envp;          /* kernel-owned NULL-terminated array */
    void  *backing;       /* kmalloc'd block holding all strings */
    size_t total;         /* combined size, ≤ 64 KiB */
};

int  exec_strings_capture(const char *user_path,
                          char *const *user_argv,
                          char *const *user_envp,
                          struct exec_strings *out);
void exec_strings_release(struct exec_strings *s);
```

- All copies are usercopy-validated. Bad pointer ⇒ `-EFAULT`.
- Combined size > 64 KiB ⇒ `-E2BIG` before any teardown.
- Empty argv (argv[0] == NULL) ⇒ `-EINVAL`.
- The captured strings outlive the old address space. They are freed
  after the new user stack is built (success) or on any execve
  failure.

### 6.4 Kernel-stack forging for fork's child

```c
int task_forge_user_return(struct task *child,
                           const struct iret_frame *parent_frame);
```

- Allocates a fresh kernel stack for the child.
- Writes a syscall-return frame on it as if the child had just
  returned from `fork`, with `rax = 0`.
- Sets `child->ctx` so that when the scheduler resumes this task, it
  unwinds through the forged frame and `iretq`s into userspace at
  the parent's RIP.
- The child's userspace RSP comes from the parent's frame and points
  into the cloned address space.
- The forged frame must satisfy the same invariants the syscall entry
  path enforces (RPL=3, IF=1, CS/SS user selectors).

---

## 7. Phase 1 — Foundation

Phase 1 introduces every piece of plumbing fork and execve depend on.
No new syscalls are exposed yet.

### 7.1 Deliverables

- Add `refcount` field to `struct file`.
- Add `file_get` / `file_put`. Update `fd_close`.
- Audit existing `file_destroy` callers and route them through
  `file_put`.
- Add `vmm_clone_space` (full deep copy).
- Add `exec_strings_capture` / `exec_strings_release`.
- Add `task_forge_user_return`.
- Add selftests for each of the above.
- No userspace-visible changes.

### 7.2 Success criteria

Booting with `selftest=1`:

1. `file_refcount_selftest`: open a file, dup-by-shared-pointer,
   close one ref, the other ref still works; close last ref, file
   is destroyed exactly once. No use-after-free.
2. `clone_space_selftest`: build an addr_space, map a few writable
   user pages with known content, clone it, mutate the source,
   confirm the clone is unaffected; mutate the clone, confirm the
   source is unaffected. Free both. No PMM leak.
3. `exec_strings_selftest`: capture a synthetic argv/envp from a
   fake user buffer, confirm round-trip; reject 65 KiB combined as
   `-E2BIG`; reject NULL argv as `-EINVAL`; reject argv with bad
   pointer as `-EFAULT`.
4. `forge_selftest`: forge a return frame on a kernel stack,
   confirm layout matches the syscall return path's invariants.
5. All v0.0.2 selftests still pass.

---

## 8. Phase 2 — `fork`

Phase 2 wires Phase 1's pieces into a working `fork()` syscall.

### 8.1 Deliverables

- Add `kernel/kernel/fork.c` and `kernel/syscall/sys_fork.c`.
- Add `JNU_SYS_fork = 11` to dispatch.
- Add `user/libjnu/fork.c`.
- Implement the fork sequence:
  1. Allocate child `struct process` (incl. fresh pid).
  2. Clone parent's addr_space via `vmm_clone_space`.
  3. Duplicate fd_table (refcount bump per slot).
  4. Allocate child kstack and `task_forge_user_return` from the
     parent's iret frame.
  5. Link child into parent's child list.
  6. Insert child task into the scheduler runnable queue.
  7. Return child pid in parent.
- Add `fork_selftest` (kernel side, fake user frame).

### 8.2 Success criteria

Booting normally:

1. `init` calls `fork()` from userspace.
2. Parent receives positive pid; child receives 0 in `rax`.
3. Both parent and child execute past the fork return site.
4. A write in the parent does not affect the child's memory and vice
   versa.
5. Both processes share file offsets through dup'd fds (verified by
   parent and child writing to fd 1 and observing interleaved
   output).
6. Parent calls `waitpid(child, &status)`, child calls `exit(N)`,
   parent observes `status == N`.
7. `waitpid` for a non-child pid returns `-ECHILD`.
8. Repeated fork/exit/wait stress (≥ 256 iterations) does not leak
   pages, kernel stacks, `struct task`, `struct process`, or
   `struct file`.

`execve` does not exist yet. The child of `fork` runs the same code
the parent was running. This is the correct intermediate state.

---

## 9. Phase 3 — `execve`

Phase 3 introduces image replacement.

### 9.1 Deliverables

- Add `kernel/kernel/execve.c` and `kernel/syscall/sys_execve.c`.
- Add `JNU_SYS_execve = 12` to dispatch.
- Retire `JNU_SYS_spawn = 9` (return `-ENOSYS`).
- Remove `kernel/syscall/sys_spawn.c` if it exists as a separate
  file. Otherwise gut its dispatch entry.
- Add `user/libjnu/execve.c`. Remove `user/libjnu/spawn.c`.
- Update `crt0.S` to pass envp to `main`.
- Implement the execve sequence:
  1. `exec_strings_capture` from user pointers.
  2. Open the path through VFS, build an `exec_image`.
  3. `elf64_validate_image`.
  4. Build a **new** `addr_space` via `vmm_create_space`.
  5. `elf64_load_image` into the new space.
  6. `elf64_setup_initial_stack` with argv + envp from the
     captured trampoline.
  7. Swap: `current->process->space = new_space`, switch CR3,
     update `user_entry`, `user_stack`.
  8. Destroy the old `addr_space`.
  9. `exec_strings_release`.
  10. Return to userspace at the new entry point.
- The swap-then-destroy ordering is mandatory. Destroying the old
  space before the new one is live is undefined behavior on the
  kernel stack (which may live in HHDM, but argv buffers must not).
- Port `user/init/main.c` from `spawn` to `fork + execve`.

### 9.2 Success criteria

Booting normally:

1. `init` calls `fork()` and the child calls `execve("/bin/hello",
   {"hello", NULL}, NULL)`.
2. The child's image is replaced. `/bin/hello` runs.
3. `/bin/hello` reads its argv and writes a recognizable message.
4. `/bin/hello` exits with status 7. Parent observes 7 via
   `waitpid`.
5. `execve` with a missing path returns `-ENOENT`; calling process
   continues.
6. `execve` with combined args > 64 KiB returns `-E2BIG`; calling
   process continues.
7. `execve` with a bad user pointer returns `-EFAULT`; calling
   process continues.
8. `execve` of an `ELF32` or `ET_DYN` binary returns `-ENOEXEC` per
   jnuspec2.md §2.5.
9. `JNU_SYS_spawn` returns `-ENOSYS` for any caller.
10. Repeated fork+execve+wait stress (≥ 256 iterations across at
    least two distinct ELF64 images) does not leak pages, file
    objects, address spaces, or pids.

When all ten are green, Phase 3 is done.

---

## 10. Phase 4 — Hardening and release

Phase 4 is not optional polish. It is prerequisite work for declaring
the ABI frozen.

### 10.1 Deliverables

- Audit fork's CR3 switch ordering. The new addr_space must be live
  before any user pointer in the child is touched.
- Audit execve's CR3 switch ordering. The old addr_space must not
  be reachable from the new task's CR3 before destruction.
- Audit TLB invalidation across address space swap.
- Audit kernel-stack lifetime across exec (the calling task keeps
  its existing kernel stack; only the user-half changes).
- Audit refcount paths for `struct file` against fork+exit races
  (single-CPU + preempt-disable invariant).
- Audit argv/envp usercopy bounds. Add a fuzz target to
  `user/fuzz/` exercising combined-size, missing-terminator, and
  bad-pointer cases.
- Page-leak audit: instrument PMM stats around fork/exec stress
  selftest, confirm `free_pages` returns to baseline.
- Remove any debug `panic_on_user_fault` style gates introduced
  earlier in Phases 1-3.
- Update `jnuspec2.md` non-goals table:
  - Remove `fork copy semantics  v0.0.2.1+`.
  - Add `Copy-on-write fork  v0.0.2.2+`.
  - Move `MINIX write support` to `v0.0.3+`.
- Update `readme.txt` to document fork+execve and the retired
  `spawn`.

### 10.2 Success criteria

1. All Phase 1, 2, 3 selftests still pass.
2. Stress harness (`make test`) runs ≥ 1024 fork/exec/wait cycles
   without panic, deadlock, or measurable PMM leak.
3. argv/envp fuzz harness completes ≥ 4096 random inputs with no
   kernel fault and no leak.
4. Panic output identifies user/kernel mode, current pid, and
   current syscall number.
5. `jnuspec2.md` is updated in the same patch series.

When all five are green, tag `v0.0.2.1`.

---

## 11. Testing

### 11.1 Boot-time selftests

New selftests:

- `file_refcount_selftest()`
- `clone_space_selftest()`
- `exec_strings_selftest()`
- `forge_selftest()`
- `fork_selftest()`        (Phase 2 in-kernel harness)
- `execve_selftest()`      (Phase 3 in-kernel harness)
- `forkexec_stress_selftest()` (Phase 4)

All gated by `selftest=1` per jnuspec2.md §17.1. No new boot flag is
introduced. Per-feature selftest flags are deferred until kconfig
exists.

### 11.2 Negative tests

Required negative cases beyond jnuspec2.md §17.2:

- `fork` with no free pids ⇒ `-EAGAIN`.
- `fork` with low memory ⇒ `-ENOMEM`, no half-built child remains.
- `execve` with bad path pointer ⇒ `-EFAULT`.
- `execve` with bad argv pointer ⇒ `-EFAULT`.
- `execve` with bad envp pointer ⇒ `-EFAULT`.
- `execve` with combined size > 64 KiB ⇒ `-E2BIG`.
- `execve` with empty argv ⇒ `-EINVAL`.
- `execve` of nonexistent path ⇒ `-ENOENT`.
- `execve` of ELF32 / `ET_DYN` ⇒ `-ENOEXEC`.
- Syscall number 9 ⇒ `-ENOSYS`.
- `waitpid` for non-child pid ⇒ `-ECHILD`.

### 11.3 QEMU tests

`make test` extends jnuspec2.md §17.3 with:

- A `forkexec` integration test that runs at least one fork+execve
  cycle from `init` into a MINIX-backed program.
- A timeout of 60 seconds (was 30 in v0.0.2; fork/exec stress
  warrants more headroom).

---

## 12. Risks and mitigations

| Risk                                              | Mitigation                                          |
| ------------------------------------------------- | --------------------------------------------------- |
| Fork leaves child half-constructed on failure     | Mandatory `goto fail_*` cleanup; Phase 1 selftests   |
| argv strings dereferenced after old space is gone | Capture-before-teardown invariant in execve         |
| CR3 switched before child's mappings are populated| Swap-then-switch ordering audit (§10.1)             |
| File refcount underflow on shared close+exit      | Single-CPU + preempt-disable; refcount selftest     |
| Kernel-stack reuse across exec corrupts callers   | Exec keeps existing kernel stack; documented in §10 |
| Forged child frame violates iret invariants       | `forge_selftest` checks RPL/IF/CS/SS bits           |
| Page leak under stress                            | PMM baseline check in §10.2                         |
| ABI drift between v0.0.2.1 and v0.0.2.2 (CoW)     | Stage A locked here; Stage B is pure MM patch       |
| CoW temptation bloats v0.0.2.1                    | §2.1 forbids; reviewers reject                       |
| envp interpretation creeps into kernel            | §2.7 forbids; userspace concern                     |

---

## 13. Release checklist

v0.0.2.1 is releasable only when:

1. `make` builds kernel, user programs, initramfs, and ISO.
2. `make test` passes the v0.0.2 suite plus the v0.0.2.1 additions.
3. `init` from initramfs runs.
4. `init` performs `fork` and observes the child.
5. `init` performs `fork + execve("/bin/hello", ...)` and observes
   the child's exit status.
6. `init` performs `fork + execve` of a MINIX-backed program and
   observes the child's exit status.
7. `JNU_SYS_spawn` returns `-ENOSYS`.
8. Stress harness passes ≥ 1024 fork/exec cycles.
9. argv/envp fuzz passes ≥ 4096 inputs.
10. `jnuspec2.md` is updated in the same patch series.
11. `readme.txt` documents fork/execve and the retired `spawn`.
12. All new files follow the comment, header, and cleanup style
    inherited from `jnuspec.md` via `jnuspec2.md`.

---

## 14. Document discipline

- Phases are sequential. Do not start Phase 2 before Phase 1
  selftests are green. Do not start Phase 3 before Phase 2 is green.
- Do not add a syscall in this release beyond `fork` and `execve`.
- Do not implement CoW or PMM refcounts in this release. CoW is
  v0.0.2.2.
- Do not reuse syscall number 9.
- Do not interpret envp in the kernel.
- If reality changes, update this document **and** `jnuspec2.md` in
  the same patch series as the code.
