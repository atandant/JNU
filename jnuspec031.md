# JNU Specification — v0.0.3.1

> **JNU** — *J is not Unix.* A monolithic, x86_64, freestanding kernel
> written from scratch in GNU C17 + Intel-syntax NASM, booted by Limine,
> licensed GPLv2.
>
> This document is the contract for v0.0.3.1 and the master prompt for
> its implementation. v0.0.3.1 is the **"MINIX writes back" release**:
> the read-only MINIX v1 reader shipped since v0.0.1 grows real write
> support without breaking on-disk compatibility.
>
> v0.0.3.1 is a **point release**. It supersedes the clauses of
> `jnuspec.md` §2.7 that lock MINIX to read-only, and inherits
> everything else from the spec chain
> `jnuspec03.md → jnuspec022.md → jnuspec021.md → jnuspec2.md →
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
| Version          | 0.0.3.1                                                   |
| Parent spec      | `jnuspec03.md` (chains all the way to `jnuspec.md`)       |
| Theme            | MINIX v1 read-write filesystem                            |
| Disk format      | MINIX v1 (`s_magic == 0x137F`), 14-char names             |
| New syscalls     | `creat`, `unlink`, `mkdir`, `rmdir`, `rename`, `fsync`, `ftruncate` |
| License          | GPLv2                                                     |

The release ships exactly three themes, in this order:

1. **Refactor.** `kernel/fs/minix.c` is split into a per-concern module
   tree under `kernel/fs/minix/`. Behavior identical to v0.0.3.
2. **Write.** Buffer cache, bitmap allocators, inode writeback, dir
   create/delete, growth + truncate, new VFS plumbing, `fsync`.
3. **Audit.** Security review, performance pass, fuzzing the on-disk
   format against a corrupted image, leak/race checks.

The gate for declaring v0.0.3.1 complete is unambiguous: a userspace
program can `open(O_CREAT) → write → close → reboot QEMU → open →
read` and observe the same bytes, with the image still mountable on
Linux's `mount -t minix -o ro`.

---

## 2. Locked decisions

These were debated and resolved for v0.0.3.1. Not open for casual
revision during implementation.

### 2.1 Release thesis

v0.0.3.1 is the **"MINIX writes back" release**. The release is
complete when JNU can:

1. Pass every v0.0.3 selftest unchanged.
2. Pass new selftests asserting (a) per-block round-trip, (b) bitmap
   allocator round-trip with no leaks across 4096 alloc/free cycles,
   (c) on-disk image cross-checks against a Linux-mounted image.
3. Survive a fuzz run that mutates 1 KiB of a clean image at random
   offsets and never panics on mount, only refuses with a logged
   `-EIO` / `-EINVAL`.
4. Leak no `struct vfs_inode`, no buffer cache slot, and no PMM page
   across 1000 `creat → write → unlink` cycles in the selftest.

### 2.2 Implementation order — three chunks

Implementation is split into three sequential chunks. **Each chunk
must merge clean and pass its named selftests before the next chunk
starts.** Out-of-order work is forbidden because chunk 2 builds on
chunk 1's module boundaries and chunk 3 audits chunk 2's data paths.

| Chunk | Theme       | Scope                                 | Status gate                                      |
| ----- | ----------- | ------------------------------------- | ------------------------------------------------ |
| **1** | Refactor    | Split `minix.c` + add buffer cache    | All v0.0.3 selftests still pass; no behavior diff |
| **2** | Write       | Bitmaps, writeback, create/delete, growth, truncate, VFS plumbing, new syscalls | New write selftests pass; cross-mount on Linux RO |
| **3** | Audit       | Security review, perf pass, fuzz, leak hunt, doc | Audit doc committed alongside the release commit |

### 2.3 Chunk 1 — Refactor (read-only, structural)

**No on-disk format change. No new behavior. No new syscalls.** This
chunk exists so chunk 2 is a series of small focused diffs instead of
edits to a 487-line monolith.

#### 2.3.1 Module split

```
kernel/fs/minix/
  internal.h    — packed structs, MINIX_BLOCK_SIZE, struct minix_priv,
                  struct minix_inode_info, prototypes shared inside fs/minix/
  super.c       — minix_mount, minix_ops table, selftest entry point
  inode.c       — minix_get_raw_inode, minix_bmap, indirect-block walks
  dir.c         — minix_lookup, minix_readdir, dir-entry decode helpers
  file.c        — minix_read
  buffer.c      — see §2.3.2 (buffer cache; lives here even though
                  long-term it may move to kernel/lib/bufcache.c)
```

#### 2.3.2 Buffer cache (read-only first)

A 64-slot LRU cache of 1 KiB blocks, keyed by `(bdev, block_no)`.
Read path becomes:

```
buf = bufcache_get(bdev, block);   // returns ref-counted, locked-down handle
memcpy(out, buf->data + offset, len);
bufcache_put(buf);
```

- **Eviction**: classic LRU with a spinlock; a slot is evictable iff
  refcount == 0 **and** dirty == false (chunk 2 sets dirty).
- **Sizing**: 64 slots × 1 KiB = 64 KiB resident, fixed allocation at
  init. No growth in v0.0.3.1.
- **Lock ordering**: `bufcache_lock` is below `pmm_lock` and above
  `vfs_inode_lock`. Document at the head of `buffer.c`.
- **No write-back yet.** Dirty list and `bufcache_sync()` are stubbed
  out in chunk 1, fully implemented in chunk 2.

#### 2.3.3 Acceptance criteria for chunk 1

- All v0.0.3 selftests pass identically (`pmm`, `vmm`, `slab`, `sched`,
  `fork`, `mmap`, `cow`, `clone_space`, `minix`, `vfs`).
- A new `bufcache_selftest()` proves: get/put round-trip, eviction
  under pressure, refcount honored against eviction.
- Hex-byte diff of `minix_selftest` log line vs. v0.0.3 is empty.
- `kernel/fs/minix.c` deleted; `Makefile` lists the five new files.

### 2.4 Chunk 2 — Write (the actual feature)

#### 2.4.1 New on-disk operations supported

| Operation       | VFS entry             | New syscall? |
| --------------- | --------------------- | ------------ |
| Create file     | `vfs_create`          | yes (`creat`, plus `open(O_CREAT)`) |
| Write to file   | `vfs_write`           | reuses `write` |
| Truncate file   | `vfs_truncate`        | yes (`ftruncate`) |
| Unlink file     | `vfs_unlink`          | yes (`unlink`) |
| Make directory  | `vfs_mkdir`           | yes (`mkdir`) |
| Remove directory| `vfs_rmdir`           | yes (`rmdir`) |
| Rename within fs| `vfs_rename`          | yes (`rename`) |
| Sync            | `vfs_fsync`           | yes (`fsync`) |

Cross-FS rename is **not** supported (returns `-EXDEV`). Hard links and
symlinks are **not** in scope for v0.0.3.1.

#### 2.4.2 Bitmap allocators

Two new files implement the imap (inode bitmap) and zmap (zone bitmap)
inside `kernel/fs/minix/bitmap.c`:

- `minix_alloc_inode(mnt) → uint32_t` — returns 0 on full.
- `minix_free_inode(mnt, ino)` — clears the bit, panics on double-free.
- `minix_alloc_zone(mnt) → uint32_t` — returns 0 on full.
- `minix_free_zone(mnt, zone)` — clears the bit, panics on double-free.

Both walk the in-memory cached imap/zmap blocks, find the first zero
bit, set it, mark the block dirty, and return. Bit-zero of each map
is reserved (matches MINIX convention).

#### 2.4.3 Inode writeback

`minix_inode_info` grows a `bool dirty` flag. Any mutator (`i_size`,
`i_zone[]`, `i_mtime`, `i_nlinks`) sets it. `vfs_close` on a dirty
inode calls `minix_write_inode(mnt, ino)` which reads the inode-table
block, overwrites the slot, and marks the buffer dirty.

#### 2.4.4 Block growth and truncate

- **Append/grow**: when a write extends past `i_size`, `minix_bmap`
  with a new `create=true` flag allocates direct, single-indirect, or
  double-indirect blocks as needed. Indirect-block creation also
  allocates and zeros a fresh zone for the indirect table.
- **Truncate**: walk the block tree, free every reachable zone via
  `minix_free_zone`, zero `i_zone[]` from the truncation point, update
  `i_size`. Free indirect tables only after their last child.

#### 2.4.5 Directory mutation

- **Create entry**: scan for the first slot with `inode == 0` and
  reuse it. If none exists, append at end of dir (which may grow the
  directory file via §2.4.4).
- **Delete entry**: zero the `inode` field; do **not** shrink the dir
  file. Leaves a recycle-able slot.
- **Atomicity**: a single 32-byte dir entry write fits in one block;
  no torn-write window across blocks.

#### 2.4.6 fsync semantics

`fsync(fd)` walks the buffer cache, writes every dirty buffer that
belongs to the file's mount, then writes any dirty inode in the inode
cache that belongs to the same mount. Returns when `block_write` has
returned for every dirtied block.

There is **no journaling** in v0.0.3.1. Crash safety is best-effort:
if power dies mid-write the on-disk image may be inconsistent. The
release notes call this out explicitly.

#### 2.4.7 mtime / ctime

Use `rtc_now_unix()` (already exposed by `kernel/drivers/rtc.c`) on:
- create → set `i_time` to now.
- write → update `i_time` to now.
- truncate → update `i_time` to now.
MINIX v1 has only one timestamp field; we use it for mtime.

#### 2.4.8 New syscalls — numbers

Linux x86_64 ABI numbers, consistent with v0.0.3 §2.2:

| Syscall    | Number |
| ---------- | ------ |
| `creat`    | 85     |
| `unlink`   | 87     |
| `mkdir`    | 83     |
| `rmdir`    | 84     |
| `rename`   | 82     |
| `fsync`    | 74     |
| `ftruncate`| 77     |

`open` gains support for `O_CREAT | O_TRUNC | O_APPEND`.

#### 2.4.9 Acceptance criteria for chunk 2

- New selftest `minix_write_selftest` covers: create, write, read-back,
  truncate-shrink, truncate-grow, unlink, mkdir, rmdir, rename within
  same dir, rename across dirs, fsync. Each round-trips through the
  buffer cache.
- A "round-tripping cross-check" selftest writes a file, reboots the
  test image (re-mounts in QEMU's `-loadvm`), reads back identical
  bytes.
- A scripted host-side test runs `mount -t minix -o ro` on the QEMU
  image after a clean shutdown and `diff`s a known good tree.
- 1000 `creat → write 4 KiB → unlink` cycles complete with no PMM page
  delta and no buffer-cache slot delta.

### 2.5 Chunk 3 — Audit

Mandatory before tagging v0.0.3.1.

#### 2.5.1 Security review

- **Path traversal**: every `vfs_lookup` rejects `..` that would escape
  mount root. Verified by selftest.
- **Bound checks**: every `bmap` result, every dir-entry inode number,
  every indirect-block pointer is checked against `s_nzones` /
  `s_ninodes`. Already half-done in v0.0.3 (`minix_bmap`'s `check`
  label) — extend to dir read and inode write.
- **TOCTOU**: confirm we never trust a re-read of the same block to
  contain the same value. If a check passes on read 1, decisions for
  the duration of that operation are made from the same buffer-cache
  reference, not a fresh read.
- **SMAP discipline**: every new userspace pointer (path, buffer)
  passes through `copy_*_user`. No bare `memcpy` against user
  pointers.
- **Quota / DoS**: cap the number of dirty buffers at 48 of 64; block
  new dirties beyond that until `bufcache_sync` drains. Prevents a
  single misbehaving process from hogging the cache.

#### 2.5.2 Performance pass

- **Buffer-cache hit rate** logged at boot's end-of-selftest summary.
  Target: >95% hit on the v0.0.3 boot trace.
- **No O(n²) directory walk**. `minix_lookup` already walks linearly;
  the buffer cache reduces re-reads, but the scan itself stays linear.
  Documented limitation, accepted for v0.0.3.1.
- **No double-zeroing**. Newly allocated zones come pre-zeroed from
  the buddy (§2.6 of `jnuspec03.md`); we do not memset them again.
- **Single-pass writeback**. `bufcache_sync` writes each dirty buffer
  exactly once per call; no temp copies.

#### 2.5.3 Fuzz

Add `scripts/fuzz_minix.sh` that:
1. Takes a clean MINIX image.
2. Mutates 1 KiB at a uniformly random offset.
3. Boots JNU against the corrupted image with `selftest=1`.
4. Asserts the kernel either mounts and runs to idle, or refuses
   mount with a clean `-EINVAL` / `-EIO` log line. Panic = test fail.
5. 200 iterations per release commit, configurable via env.

#### 2.5.4 Leak hunt

- PMM stat delta zero across the full selftest suite (already
  asserted; extend the assertion to cover the new selftests).
- Buffer cache slot count returns to 0 dirty + 64 free after
  `bufcache_sync_all` is called at end of selftest.
- `struct vfs_inode` count returns to 1 (the root inode) after
  selftest cleanup.

#### 2.5.5 Audit deliverable

A new file `audits/v0.0.3.1.md` is committed alongside the release
tag. Sections (in order): security findings, performance findings,
fuzz results, leak findings, deferred items list. Empty sections are
explicit (`No findings.`), never omitted.

### 2.6 Coding style and constraints

Inherited unchanged from `jnuspec.md` §3 / §4 — same compiler flags,
same style, same forbidden-function list (no malloc, no libc, no
floating-point math at file scope, etc.). New rules specific to
v0.0.3.1:

- **No bare `block_read` / `block_write` from FS code.** All disk I/O
  goes through `bufcache_get` / `bufcache_put` so the cache is the
  single source of truth for block contents.
- **No on-disk format extension.** v0.0.3.1 stays bit-identical to
  MINIX v1; do not invent JNU-specific fields.
- **No partial writes left dirty across a syscall return.** If a write
  must fail halfway, roll back the in-memory inode and bitmap state
  so the on-disk view stays consistent the moment the caller observes
  failure.
- **Locks (chunk 1+2)**: `bufcache_lock` < `vfs_inode_lock` <
  `mnt->lock`. Top-down acquire only. Documented at the head of
  every file that takes more than one of these.

---

## 3. Selftest matrix

| Test                          | Lives in                    | Chunk | Must pass for tag |
| ----------------------------- | --------------------------- | ----- | ----------------- |
| `bufcache_selftest`           | `kernel/fs/minix/buffer.c`  | 1     | yes               |
| `minix_selftest` (existing)   | `kernel/fs/minix/super.c`   | 1     | yes               |
| `minix_bitmap_selftest`       | `kernel/fs/minix/bitmap.c`  | 2     | yes               |
| `minix_write_selftest`        | `kernel/fs/minix/file.c`    | 2     | yes               |
| `minix_dir_selftest`          | `kernel/fs/minix/dir.c`     | 2     | yes               |
| `minix_fsync_selftest`        | `kernel/fs/minix/super.c`   | 2     | yes               |
| `scripts/fuzz_minix.sh` (200 iters) | host script           | 3     | yes               |
| `scripts/cross_mount.sh`      | host script                 | 3     | yes               |

All selftests are gated on `selftest=1` cmdline (inherited from
`jnuspec.md` §2.12). Fuzz and cross-mount are host scripts, not
boot-time.

---

## 4. Out of scope (deferred to v0.0.4 or later)

- Page cache integrated with mmap'd file-backed mappings.
- Journaling, ordered writes, crash safety beyond best-effort.
- MINIX v2 / v3 (30-character names, larger zones).
- Multiple simultaneous mounts of MINIX.
- Symlinks, hard links beyond the implicit `.` / `..`.
- ACLs, xattrs, mode bits beyond what MINIX v1 already encodes.
- Quotas.
- A real `/dev` filesystem (still synthetic via `resolve_dev_chardev`).

---

## 5. Release checklist

- [ ] Chunk 1 merged; all v0.0.3 selftests pass; `bufcache_selftest`
      passes.
- [ ] Chunk 2 merged; all chunk-2 selftests pass; cross-mount script
      passes against a clean shutdown image.
- [ ] Chunk 3 audit doc committed; fuzz run logged; no leak findings
      open.
- [ ] `JNU_VERSION` bumped to `0.0.3.1` in build metadata.
- [ ] `readme` updated; release notes call out best-effort crash
      safety.
- [ ] Tag pushed.

---

*End of jnuspec031.md.*
