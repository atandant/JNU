# JNU Specification - v0.0.2

> **JNU** - *J is not Unix.* A monolithic, x86_64, freestanding kernel
> written from scratch in GNU C17 + Intel-syntax NASM, booted by Limine,
> licensed GPLv2.
>
> This document is the contract for v0.0.2 and the master prompt for its
> implementation. v0.0.2 is the first userspace release: it introduces
> preemptive scheduling, initramfs, ELF64 execution, a small native
> syscall ABI, and security hardening required by ring 3.
>
> Do not silently deviate. If this document disagrees with reality on the
> ground, fix the spec or fix the code, never neither.

---

## 1. Identity and scope

| Property         | Value                                      |
| ---------------- | ------------------------------------------ |
| Name             | JNU (J is not Unix)                        |
| Version          | 0.0.2                                      |
| Architecture     | x86_64 only                                |
| Bootloader       | Limine (Limine Boot Protocol)              |
| Languages        | GNU C17, Intel-syntax NASM                 |
| Compiler         | clang (`--target=x86_64-unknown-none-elf`) |
| Linker           | lld                                        |
| Runtime builtins | compiler-rt                                |
| Kernel design    | Monolithic, strict subsystem boundaries    |
| Userspace ABI    | Native JNU ABI, Unix-inspired, not Linux   |
| Init program     | `/init` from initramfs                     |
| Initramfs format | `cpio` `newc`                              |
| Executable format| ELF64, static `ET_EXEC`                    |
| Scheduler        | Preemptive round-robin, single CPU         |
| License          | GPLv2                                      |
| Test target      | `qemu-system-x86_64 -machine q35 -m 256M`  |

JNU is from scratch. No Unix or Linux source code is copied or adapted.
JNU may borrow concepts from Unix-family systems, but the ABI and
implementation are native unless this document explicitly says otherwise.

---

## 2. Locked decisions

These were debated and resolved for v0.0.2. They are not open for casual
revision during implementation.

### 2.1 Release thesis

v0.0.2 is the **userspace release**.

The release is complete when JNU can:

1. Boot with a Limine-provided initramfs.
2. Parse `cpio` `newc`.
3. Load `/init` as a static ELF64 userspace program.
4. Enter ring 3 safely.
5. Handle a small native syscall set.
6. Run more than one task under preemptive round-robin scheduling.
7. Execute an ELF64 program from MINIX through the same exec path used
   for initramfs.

Anything that does not support these goals is suspect.

### 2.2 Non-goals

The following are explicitly out of scope for v0.0.2:

| Feature                         | Target        |
| ------------------------------- | ------------- |
| ELF32 / x86 compatibility mode  | v0.0.3+       |
| Dynamic linking / `PT_INTERP`   | v0.0.3+       |
| PIE / ASLR                      | v0.0.3+       |
| Linux syscall compatibility     | v0.0.4+       |
| `fork` copy semantics           | v0.0.2.1+     |
| MLFQ scheduler                  | v0.0.4+       |
| Signals                         | v0.0.3.2+     |
| Futexes & Mutexes               | v0.0.3+       |
| Networking                      | v0.0.5+       |
| AHCI / SATA                     | v0.0.3+       |
| HPET                            | v0.0.3+       |
| Copy-on-write fork              | v0.0.2.2+     |
| MINIX write support             | v0.0.3+       |
| SMP / IPIs / TLB shootdown      | v0.0.4+       |
| Loadable modules                | v0.0.5+       |

Do not smuggle these in under "small optional feature" language.

### 2.3 Kernel design

- Monolithic kernel. All core services run in kernel space.
- Strict subsystem boundaries. Internal structures are not accessed
  across subsystems except through headers in `kernel/include/jnu/`.
- No driver model yet. `struct block_device` and `struct char_device`
  remain the only device abstractions.
- Userspace is untrusted. Every pointer, length, path, fd, and flags
  value crossing the syscall boundary is hostile until validated.

### 2.4 Userspace ABI policy

JNU's syscall ABI is native.

- It is Unix-inspired: file descriptors, paths, processes, exit status,
  and `errno`-style negative kernel errors.
- It is not Linux-number-compatible.
- It does not promise Linux flag compatibility.
- A future Linux compatibility layer may translate Linux ABI calls into
  native JNU calls, but native JNU remains the kernel's primary ABI.

### 2.5 Executable policy

- v0.0.2 supports ELF64 only.
- v0.0.2 supports static `ET_EXEC` only.
- `ET_DYN`, PIE, dynamic linking, `PT_INTERP`, TLS, and ELF32 are
  rejected with `-ENOEXEC`.
- The ELF loader may load from initramfs or VFS, but the mapping logic is
  shared. There must not be one ELF loader for initramfs and a different
  ELF loader for MINIX.

### 2.6 Initramfs policy

- Initramfs is mandatory for v0.0.2 boot.
- Format is `cpio` `newc`.
- `/init` is loaded from initramfs.
- Initramfs is read-only.
- Initramfs exists to decouple first userspace from ATA/MINIX.
- MINIX execution is supported later in the boot flow through VFS.

### 2.7 Scheduler policy

v0.0.2 uses preemptive round-robin.

- One CPU.
- One runnable queue.
- 10 ms quantum.
- LAPIC timer is the scheduler tick.
- PIT may remain for calibration or legacy timekeeping, but it is not the
  scheduler tick source once LAPIC timer scheduling is live.
- MLFQ is explicitly deferred. The scheduler's data structures should
  avoid blocking MLFQ later, but v0.0.2 must not implement MLFQ.

### 2.8 Security policy

Security hardening in v0.0.2 is not optional polish. It is prerequisite
work for userspace.

Required:

- User/kernel address split is enforced.
- Page zeroing on allocation for userspace-visible pages.
- Kernel mappings are supervisor-only.
- User mappings use `PTE_USER`.
- W^X is enforced for ELF mappings.
- HHDM remains NX.
- `CR0.WP` remains enabled.
- NX remains required.
- SMEP and SMAP are enabled when CPU support exists.
- Syscall entry never trusts user registers beyond the ABI contract.
- Kernel never directly dereferences user pointers outside usercopy
  helpers.
- Debug boot flags that dump memory or block contents are documented as
  debug-only and must never be reachable from userspace.

---

## 3. Coding style

v0.0.2 inherits the coding style, commenting policy, and file header
policy from `jnuspec.md`.

Important v0.0.2 clarifications:

- GNU C17 only.
- Intel-syntax NASM only.
- Tabs for indentation in C source, matching the existing kernel style.
- Comments explain contracts, ordering constraints, hardware rules, and
  security assumptions. They do not narrate obvious assignments.
- Public subsystem APIs live in `kernel/include/jnu/`.
- Internal helpers are `static`.
- Every resource-acquiring function with more than one cleanup path uses
  the canonical `goto fail_*` cleanup idiom.

### 3.1 `goto` cleanup rule

The `goto` cleanup rule is mandatory for:

- ELF loading.
- Process creation.
- Address-space construction.
- File descriptor allocation.
- Initramfs parser setup.
- Syscall handlers that acquire multiple references.

Example shape:

```c
int process_spawn(const char *path, struct process **out)
{
	struct process *p;
	struct addr_space *space;
	int err;

	p = process_alloc();
	if (!p) {
		return -ENOMEM;
	}

	space = vmm_create_space();
	if (!space) {
		err = -ENOMEM;
		goto fail_process;
	}

	err = exec_load_path(path, space, p);
	if (err) {
		goto fail_space;
	}

	*out = p;
	return 0;

fail_space:
	vmm_destroy_space(space);
fail_process:
	process_free(p);
	return err;
}
```

Rules:

- `goto` only jumps forward to cleanup labels at the end of the function.
- Labels are ordered in reverse acquisition order.
- Labels are named after the resource being unwound.
- No `goto` crosses function boundaries.
- No cleanup label performs unrelated work.

---

## 4. Repository structure

v0.0.2 extends the existing tree without a large reorganization.

### 4.1 New kernel directories

```
kernel/exec/
kernel/initramfs/
kernel/syscall/
kernel/user/
```

### 4.2 New architecture files

```
kernel/arch/x86_64/context.S
kernel/arch/x86_64/syscall_entry.S
kernel/arch/x86_64/usermode.c
```

### 4.3 New kernel source files

```
kernel/exec/elf64.c

kernel/initramfs/cpio_newc.c
kernel/initramfs/initramfs.c

kernel/syscall/common.c
kernel/syscall/dispatch.c
kernel/syscall/sys_close.c
kernel/syscall/sys_exit.c
kernel/syscall/sys_fstat.c
kernel/syscall/sys_getpid.c
kernel/syscall/sys_lseek.c
kernel/syscall/sys_open.c
kernel/syscall/sys_read.c
kernel/syscall/sys_spawn.c
kernel/syscall/sys_waitpid.c
kernel/syscall/sys_write.c
kernel/syscall/sys_yield.c

kernel/user/copy.c
kernel/user/fd.c
kernel/user/process.c
```

One syscall per file is the default. Shared mechanics must stay in
`common.c` or in subsystem helpers. Do not duplicate user pointer
validation, fd lookup, path copying, or task wakeup logic across syscall
files.

### 4.4 New kernel headers

```
kernel/include/jnu/arch_syscall.h
kernel/include/jnu/context.h
kernel/include/jnu/cpio_newc.h
kernel/include/jnu/elf64.h
kernel/include/jnu/exec.h
kernel/include/jnu/fd.h
kernel/include/jnu/initramfs.h
kernel/include/jnu/process.h
kernel/include/jnu/syscall.h
kernel/include/jnu/syscall_nr.h
kernel/include/jnu/usercopy.h
```

### 4.5 New userland tree

```
user/init/main.c
user/libjnu/crt0.S
user/libjnu/syscall.S
user/libjnu/include/jnu_syscall.h
user/libjnu/close.c
user/libjnu/exit.c
user/libjnu/fstat.c
user/libjnu/getpid.c
user/libjnu/lseek.c
user/libjnu/open.c
user/libjnu/read.c
user/libjnu/spawn.c
user/libjnu/waitpid.c
user/libjnu/write.c
user/libjnu/yield.c
```

`libjnu` is not libc. It is a tiny native syscall wrapper library.

### 4.6 New scripts and build output

```
scripts/make-initramfs.sh
scripts/build-user.sh

build/user/init
build/initramfs.cpio
```

The ISO contains:

```
boot/kernel.elf
boot/initramfs.cpio
boot/limine.cfg
```

Limine loads `initramfs.cpio` as a module.

---

## 5. Syscall ABI

### 5.1 Entry mechanism

v0.0.2 uses `syscall` / `sysret` if available. x86_64 requires MSR
setup during CPU initialization:

- `IA32_EFER.SCE` enabled.
- `IA32_STAR` configured for kernel/user code segments.
- `IA32_LSTAR` points to `syscall_entry`.
- `IA32_FMASK` masks unsafe flags on entry.

If `syscall` / `sysret` bring-up blocks the release, `int 0x80` may be
used temporarily behind the same C dispatcher, but the spec target is
`syscall` / `sysret`.

### 5.2 Register ABI

Native JNU syscall register ABI:

| Register | Meaning          |
| -------- | ---------------- |
| `rax`    | syscall number   |
| `rdi`    | arg0             |
| `rsi`    | arg1             |
| `rdx`    | arg2             |
| `r10`    | arg3             |
| `r8`     | arg4             |
| `r9`     | arg5             |
| `rax`    | return value     |

Return values:

- `>= 0`: success.
- `< 0`: negative kernel errno.

The kernel does not set a userspace `errno` variable. `libjnu` may do
that later; v0.0.2 wrappers may return raw negative errors.

### 5.3 Syscall numbers

```
#define JNU_SYS_exit		0
#define JNU_SYS_write		1
#define JNU_SYS_read		2
#define JNU_SYS_open		3
#define JNU_SYS_close		4
#define JNU_SYS_lseek		5
#define JNU_SYS_getpid		6
#define JNU_SYS_yield		7
#define JNU_SYS_fstat		8
#define JNU_SYS_spawn		9
#define JNU_SYS_waitpid		10
```

There is no `fork` in v0.0.2. `spawn` is the process creation syscall.

### 5.4 Required syscall files

Each syscall has one primary implementation file:

| Syscall | File                         |
| ------- | ---------------------------- |
| exit    | `kernel/syscall/sys_exit.c`   |
| write   | `kernel/syscall/sys_write.c`  |
| read    | `kernel/syscall/sys_read.c`   |
| open    | `kernel/syscall/sys_open.c`   |
| close   | `kernel/syscall/sys_close.c`  |
| lseek   | `kernel/syscall/sys_lseek.c`  |
| getpid  | `kernel/syscall/sys_getpid.c` |
| yield   | `kernel/syscall/sys_yield.c`  |
| fstat   | `kernel/syscall/sys_fstat.c`  |
| spawn   | `kernel/syscall/sys_spawn.c`  |
| waitpid | `kernel/syscall/sys_waitpid.c`|

Shared helpers:

| Helper area       | File                         |
| ----------------- | ---------------------------- |
| dispatcher        | `kernel/syscall/dispatch.c`   |
| shared validation | `kernel/syscall/common.c`     |
| syscall numbers   | `kernel/include/jnu/syscall_nr.h` |
| syscall prototypes| `kernel/include/jnu/syscall.h` |

### 5.5 Syscall semantics

#### `exit(status)`

```
void exit(int status);
```

- Terminates the current process.
- Stores low 8 bits of `status` as exit status.
- Closes file descriptors.
- Wakes parent waiters.
- Enters `TASK_ZOMBIE`.
- Never returns.

#### `write(fd, buf, len)`

```
ssize_t write(int fd, const void *buf, size_t len);
```

- Validates `buf..buf+len` with `copy_from_user`.
- Supports stdout/stderr console fds in Phase 3.
- Supports VFS writable files only when a filesystem supports writes.
- Returns bytes written or negative errno.

#### `read(fd, buf, len)`

```
ssize_t read(int fd, void *buf, size_t len);
```

- Validates destination with `copy_to_user`.
- Supports stdin keyboard input if available.
- Supports VFS file reads.
- Returns bytes read, zero on EOF, or negative errno.

#### `open(path, flags)`

```
int open(const char *path, int flags);
```

- Copies a NUL-terminated path from userspace with a bounded helper.
- v0.0.2 supports read-only open first.
- Unknown flags return `-EINVAL`.
- Opening from initramfs and MINIX is routed through VFS or a shared
  file abstraction.

#### `close(fd)`

```
int close(int fd);
```

- Releases a process fd table entry.
- Drops file/inode references.
- Returns `0` or negative errno.

#### `lseek(fd, off, whence)`

```
int64_t lseek(int fd, int64_t off, int whence);
```

- Supports `SEEK_SET`, `SEEK_CUR`, `SEEK_END`.
- Rejects negative resulting offsets.
- Returns new offset or negative errno.

#### `getpid()`

```
int getpid(void);
```

- Returns current process id.

#### `yield()`

```
int yield(void);
```

- Voluntarily yields the CPU.
- Returns `0`.

#### `fstat(fd, st)`

```
int fstat(int fd, struct jnu_stat *st);
```

- Copies a compact native stat structure to userspace.
- The native structure is not Linux `struct stat`.
- Must include at least: inode number, size, mode, and file type.

#### `spawn(path, argv)`

```
int spawn(const char *path, char *const argv[]);
```

- Creates a new process from an executable path.
- Does not clone the caller's address space.
- Inherits standard fds `0`, `1`, `2`.
- `argv` support may be minimal in early Phase 3 but must be bounded and
  validated before final v0.0.2.
- Returns child pid or negative errno.

#### `waitpid(pid, status)`

```
int waitpid(int pid, int *status);
```

- Waits for a child process to exit.
- `pid > 0`: wait for specific child.
- `pid == -1`: wait for any child.
- Copies exit status to userspace if `status != NULL`.
- Returns child pid or negative errno.

---

## 6. Process and scheduler model

### 6.1 Task states

```
enum task_state {
	TASK_RUNNABLE,
	TASK_RUNNING,
	TASK_SLEEPING,
	TASK_ZOMBIE,
};
```

### 6.2 Core structures

`struct task` is the schedulable entity.

Required fields:

- pid / tid.
- state.
- saved kernel context.
- kernel stack base/top.
- current address space.
- parent pointer.
- exit status.
- run queue links.
- wait queue links or equivalent.

`struct process` owns process-level resources.

Required fields:

- pid.
- main task pointer.
- address space.
- fd table.
- parent process.
- child list.
- exit status.
- process state.

In v0.0.2, one process has one task. The split exists so threads can be
added later without turning the process object into a dumping ground.

### 6.3 Context switching

`kernel/arch/x86_64/context.S` implements low-level switch mechanics.

The switch must preserve all callee-saved registers required by the
chosen C ABI and enough machine state to resume kernel execution.

FPU/SSE/AVX state is not saved in v0.0.2 because the kernel is built
with `-mno-sse -mno-mmx -mno-sse2 -mgeneral-regs-only` and userspace
floating-point support is deferred.

### 6.4 Timer preemption

- LAPIC timer vector is 32.
- Quantum is 10 ms.
- Timer interrupt acknowledges LAPIC EOI.
- If current task exhausts quantum and preemption is enabled, scheduler
  selects the next runnable task.
- Preemption is disabled while holding spinlocks.

### 6.5 Idle task

The idle task is a real task.

Idle loop:

```c
for (;;) {
	asm volatile ("sti; hlt; cli");
}
```

The scheduler never returns `NULL`; if no process is runnable, it
schedules idle.

### 6.6 Blocking and wakeup

v0.0.2 needs minimal sleep/wakeup for `waitpid` and possibly keyboard
input.

Rules:

- Sleeping task state is `TASK_SLEEPING`.
- Wakeup moves task to `TASK_RUNNABLE`.
- Wakeup paths must be safe from interrupt context when used by drivers.
- No mutex abstraction in v0.0.2 unless a concrete blocking lock is
  forced by implementation.

---

## 7. Memory and usercopy

### 7.1 User address range

User pointers must satisfy:

- Canonical low-half address.
- Non-NULL unless syscall explicitly permits NULL.
- Entire range lies below `0x0000800000000000`.
- Range addition does not overflow.
- Mapped with `PTE_USER`.
- Permission matches operation: read for `copy_from_user`, write for
  `copy_to_user`.

### 7.2 Usercopy helpers

Required API:

```
int copy_from_user(void *dst, const void *usrc, size_t len);
int copy_to_user(void *udst, const void *src, size_t len);
int copy_string_from_user(char *dst, const char *usrc, size_t max);
```

Rules:

- No syscall handler directly dereferences user memory.
- Usercopy handles zero-length copies.
- Usercopy rejects overflowed ranges.
- Usercopy returns negative errno.
- If SMAP is enabled, usercopy performs the required `stac` / `clac`
  sequencing in the narrowest possible window.

### 7.3 Page zeroing

Any physical page that becomes visible to userspace must be zeroed
before mapping.

Acceptable implementation choices:

- Zero on allocation for user pages.
- Zero in the ELF loader before copying segment bytes.
- Zero in a `pmm_alloc_user_page()` helper.

The contract is about behavior: userspace must never observe stale
kernel, bootloader, or previous-process memory.

### 7.4 Address spaces

Each process has a private PML4.

- Kernel half is shared.
- User half starts empty.
- NULL page is never mapped.
- User stacks are mapped below the user top.
- ELF load addresses must be below user top and above page zero.
- `vmm_destroy_space()` releases user mappings and page tables owned by
  the process. It must not release shared kernel mappings.

---

## 8. Initramfs

### 8.1 Limine module handling

Limine must load `boot/initramfs.cpio` as a module.

The kernel locates the module by:

- command line tag if available, or
- first module if only one module exists.

If no initramfs is present, v0.0.2 boot fails with a clear panic.

### 8.2 `cpio` `newc` parser

The parser supports:

- `070701` magic.
- Regular files.
- Directories.
- File names.
- 4-byte alignment.
- `TRAILER!!!`.

The parser rejects:

- bad magic.
- overflowing sizes.
- headers that run beyond the module.
- file data that runs beyond the module.
- absolute confusion such as empty names or malformed traversal.

The parser may ignore ownership, timestamps, and permissions except for
file type bits needed by lookup.

### 8.3 Initramfs API

Required API:

```
int initramfs_init(void *base, size_t len);
int initramfs_lookup(const char *path, struct initramfs_file *out);
ssize_t initramfs_read_at(const struct initramfs_file *file,
			  uint64_t off, void *buf, size_t len);
```

The implementation is read-only.

---

## 9. ELF64 execution

### 9.1 Loader input abstraction

The ELF loader consumes a generic image reader:

```
struct exec_image {
	ssize_t (*read_at)(void *ctx, uint64_t off, void *buf, size_t len);
	uint64_t size;
	void *ctx;
};
```

Both initramfs and VFS/MINIX execution use this abstraction.

### 9.2 ELF validation

The loader validates:

- ELF magic.
- `EI_CLASS == ELFCLASS64`.
- `EI_DATA == ELFDATA2LSB`.
- `e_machine == EM_X86_64`.
- `e_type == ET_EXEC`.
- sane `e_phoff`, `e_phnum`, `e_phentsize`.
- all `PT_LOAD` ranges are canonical user addresses.
- no load segment maps page zero.
- no load segment enters kernel half.
- no overflow in `p_vaddr + p_memsz`.
- no overlap between load segments.
- `p_filesz <= p_memsz`.

Unsupported features return `-ENOEXEC`.

### 9.3 Mapping rules

For each `PT_LOAD`:

- Allocate zeroed pages.
- Map with `PTE_USER`.
- Apply write permission only when `PF_W` is set.
- Apply execute permission only when `PF_X` is set.
- Enforce W^X: a segment must not be both writable and executable.
- Copy file bytes.
- Leave `p_memsz - p_filesz` zeroed.

The loader must use `goto fail_*` cleanup so partially-loaded processes
do not leak pages or page tables.

### 9.4 Initial user stack

Minimum stack:

- At least 64 KiB mapped.
- Guard page below the stack.
- 16-byte alignment before user entry.
- `argc`, `argv`, and strings supported by final v0.0.2.

Early bring-up may enter `/init` with `argc == 0`, but final v0.0.2 must
support bounded argv for `spawn`.

### 9.5 Entering userspace

`kernel/arch/x86_64/usermode.c` provides the final transition.

Requirements:

- User CS/SS selectors use ring 3 descriptors.
- Initial `RFLAGS` has IF enabled and reserved bit set.
- RSP points to user stack.
- RIP points to ELF entry.
- TSS RSP0 points to the current task's kernel stack.
- Returning from userspace to kernel updates/reuses the correct kernel
  stack for the current task.

---

## 10. Files and file descriptors

### 10.1 FD table

Each process owns an fd table.

Minimum:

- 32 fds per process.
- fd `0`: stdin.
- fd `1`: stdout.
- fd `2`: stderr.
- fds are allocated lowest-numbered first.

### 10.2 File object

A file object tracks:

- backing object type.
- current offset.
- access flags.
- reference count.
- operations table.

Backing object types:

- console/keyboard character device.
- initramfs file.
- VFS file, including MINIX.

### 10.3 VFS execution

Phase 4 supports executing ELF64 files from MINIX through VFS.

The ELF loader must not know about MINIX. It receives an `exec_image`
backed by VFS `read_at`.

---

## 11. Build system

### 11.1 Kernel version

Build metadata changes to:

```
const char jnu_version[] = "0.0.2";
```

### 11.2 User build

User programs are built freestanding.

Required flags:

- `--target=x86_64-unknown-none-elf`
- `-ffreestanding`
- `-fno-pic -fno-pie`
- `-nostdlib`
- no libc dependency

The user linker script places `/init` at a stable low-half virtual
address, typically `0x0000000000400000`.

### 11.3 Initramfs build

`make` builds:

1. kernel ELF.
2. user `/init`.
3. `build/initramfs.cpio`.
4. bootable ISO.

The initramfs includes at least:

```
/init
/bin/hello
/etc/motd
```

`/bin/hello` may be used to test `spawn` from initramfs before MINIX
execution is complete.

---

## 12. Kernel command line

v0.0.2 honors v0.0.1 flags plus:

| Key             | Meaning                                  |
| --------------- | ---------------------------------------- |
| `init=<path>`   | override init path, default `/init`      |
| `noinit=1`      | do not enter userspace, for debugging    |
| `schedtrace=1`  | log scheduler decisions                  |
| `syscalltrace=1`| log syscall number and return value      |
| `userpanic=1`   | run deliberate userspace fault test      |

Trace flags are debug aids and must be quiet by default.

---

## 13. Phase 1 - Hardening and boot substrate

Phase 1 prepares the kernel for hostile userspace.

### 13.1 Deliverables

- Update version metadata to `0.0.2`.
- Add Limine module request support for initramfs.
- Add `kernel/initramfs/cpio_newc.c`.
- Add `kernel/initramfs/initramfs.c`.
- Add initramfs build script.
- Add page zeroing path for userspace pages.
- Add user address validation helpers.
- Add `copy_from_user`, `copy_to_user`, and string copy helpers.
- Audit obvious direct user pointer hazards before syscalls exist.
- Tighten ACPI/MADT parser bounds checks where needed.
- Tighten MINIX parser checks for malformed disk images.

### 13.2 Success criteria

Booting with `selftest=1`:

1. Kernel finds Limine initramfs module.
2. `cpio newc` parser lists `/init`.
3. Initramfs malformed-header selftest rejects bad archive data.
4. Usercopy selftest rejects kernel-half and overflowed ranges.
5. PMM/VMM selftests still pass.
6. Existing VFS/MINIX read-only tests still pass.

---

## 14. Phase 2 - Scheduler and process core

Phase 2 makes multiple schedulable contexts real before ring 3.

### 14.1 Deliverables

- Replace `sched.c` stub with round-robin scheduler.
- Add `struct task`.
- Add `struct process`.
- Add PID allocator.
- Add kernel stack allocation/free.
- Add low-level context switch.
- Add idle task.
- Convert boot thread into initial kernel task.
- Add LAPIC timer scheduler tick.
- Add `sched_yield`.
- Add minimal sleep/wakeup for wait paths.
- Add process/fd scaffolding.

### 14.2 Success criteria

Booting with `selftest=1`:

1. Scheduler initializes an idle task and boot task.
2. Kernel can create two test kernel threads.
3. Timer preemption switches between test threads.
4. `sched_yield` switches voluntarily.
5. No PMM/slab leak after thread selftest.
6. Panic output includes current pid/task id.

---

## 15. Phase 3 - Syscalls and initramfs userspace

Phase 3 crosses into ring 3 and runs `/init`.

### 15.1 Deliverables

- Add syscall MSR setup.
- Add `syscall_entry.S`.
- Add syscall dispatcher.
- Add one implementation file per syscall.
- Add user `libjnu`.
- Add `user/init/main.c`.
- Add ELF64 loader for static `ET_EXEC`.
- Add user stack setup.
- Add process creation from initramfs file.
- Add standard fds.
- Enter `/init` from initramfs.

### 15.2 Required `/init` behavior

The initial `/init` program must:

1. Write a boot message to stdout.
2. Print its pid.
3. Spawn `/bin/hello` from initramfs.
4. Wait for `/bin/hello`.
5. Exit with status 0 or idle by yielding.

### 15.3 Success criteria

Booting normally:

1. Kernel loads `/init` from initramfs.
2. Kernel enters ring 3.
3. `/init` writes to stdout via syscall.
4. `/init` calls `getpid`.
5. `/init` spawns `/bin/hello`.
6. Parent waits and receives child exit status.
7. Bad syscall number returns `-ENOSYS`.
8. Bad user pointer returns `-EFAULT`.
9. Deliberate user page fault kills the user process or panics with a
   clear user fault report, according to the selected early policy.

Final v0.0.2 should prefer killing the offending process when possible.
During early bring-up, panic is acceptable if documented and gated by
phase.

---

## 16. Phase 4 - VFS/MINIX execution and release polish

Phase 4 proves execution is not initramfs-specific.

### 16.1 Deliverables

- Add VFS-backed `exec_image`.
- Execute ELF64 from MINIX read-only filesystem.
- Allow `/init` to spawn a MINIX-backed program.
- Add fd support for VFS regular file reads.
- Add `fstat`.
- Add `lseek`.
- Add boot tests for initramfs and MINIX execution.
- Clean up debug-only panics from earlier phases.
- Document remaining limitations.

### 16.2 Success criteria

With an ATA disk containing a MINIX root:

1. Kernel mounts MINIX root.
2. `/init` from initramfs runs.
3. `/init` opens a MINIX file and reads it.
4. `/init` spawns a MINIX-backed ELF64 program.
5. The child writes to stdout and exits.
6. Parent observes correct exit status.
7. `selftest=1` passes.
8. No known page leaks across repeated spawn/wait tests.

When all eight are green, tag `v0.0.2`.

---

## 17. Testing

### 17.1 Boot-time selftests

New selftests:

- `initramfs_selftest()`.
- `usercopy_selftest()`.
- `sched_selftest()`.
- `process_selftest()`.
- `elf64_selftest()`.
- `syscall_selftest()`.

Selftests are runtime-gated by `selftest=1`.

### 17.2 Negative tests

Required negative cases:

- malformed `newc` magic.
- `newc` file size overflow.
- ELF bad magic.
- ELF32 rejected.
- ELF64 `ET_DYN` rejected.
- ELF segment in kernel half rejected.
- ELF writable+executable segment rejected.
- syscall bad number.
- syscall bad pointer.
- open nonexistent path.
- wait for non-child pid.

### 17.3 QEMU tests

`make test` should eventually:

- build ISO.
- boot QEMU with serial output.
- pass `selftest=1`.
- fail on panic.
- fail on `[FAIL]`.
- timeout after 30 seconds.

---

## 18. Risks and mitigations

| Risk                                         | Mitigation                                      |
| -------------------------------------------- | ----------------------------------------------- |
| Syscall entry corrupts kernel stack          | TSS RSP0 selftest, deliberate syscall stress    |
| User pointer dereferenced directly           | usercopy-only rule, syscall review checklist    |
| ELF loader leaks pages on partial failure    | mandatory `goto fail_*` cleanup                 |
| Scheduler preempts unsafe global state       | spinlock/IRQ audit, preempt disable in locks    |
| Page table ownership confusion               | clear user/kernel PML4 ownership rules          |
| Initramfs parser overflow                    | bounded arithmetic, malformed archive tests     |
| MINIX execution couples loader to filesystem | `exec_image` abstraction                        |
| MLFQ temptation bloats release               | round-robin locked for v0.0.2                   |
| Linux ABI temptation bloats syscall table    | native ABI locked, Linux compat deferred        |

---

## 19. Release checklist

v0.0.2 is releasable only when:

1. `make` builds kernel, user programs, initramfs, and ISO.
2. `make test` passes or an equivalent documented QEMU selftest pass is
   recorded.
3. `/init` runs from initramfs.
4. A child program runs from initramfs.
5. A child program runs from MINIX.
6. Bad syscalls and bad user pointers fail safely.
7. Repeated spawn/wait tests do not leak pages.
8. Panic output identifies user/kernel mode and current process.
9. `readme.txt` documents new build/run steps.
10. All new files follow the comment and cleanup style inherited from
    `jnuspec.md`.

---

## 20. Document discipline

- Every phase is sequential.
- Do not start Phase 2 before Phase 1 success criteria are green.
- Do not enter userspace before scheduler/process basics work in kernel
  selftests.
- Do not add a syscall without documenting its number, ABI, file, and
  validation rules here.
- Do not add Linux compatibility in v0.0.2.
- Do not add ELF32 in v0.0.2.
- Do not add MLFQ in v0.0.2.
- If reality changes, update this document in the same patch series as
  the code.
