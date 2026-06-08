/*
 * include/jnu/user/fd.h - File descriptor table scaffolding.
 *
 * `struct file` is the open-file description: type tag, current offset,
 * mode flags, refcount, and a union over backing types. The fd table
 * stores `struct file *` slots; multiple fd slots (across one or more
 * processes) may share a `struct file` once refcount > 1, so close,
 * exit, and fork must move references via `file_get` / `file_put`
 * rather than freeing directly.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/base/types.h>
#include <jnu/drivers/chardev.h>
#include <jnu/fs/initramfs.h>
#include <jnu/fs/vfs.h>
#include <jnu/lib/mutex.h>
#include <uapi/jnu/stat.h>

#define JNU_MAX_FDS 32

enum jnu_file_type {
	JNU_FILE_INITRAMFS,
	JNU_FILE_VFS,
	JNU_FILE_CHARDEV,
};

/*
 * Newly allocated `struct file` objects MUST have refcount = 1 before
 * being inserted into an fd table. The slot owns that initial
 * reference; `fd_alloc` does not bump refcount. `fd_close` drops the
 * slot's reference via `file_put`.
 *
 * `file_get` / `file_put` are the only sanctioned ways to share or
 * release a file across fd slots and processes. v0.0.2.1 is single-CPU
 * with preempt-disable around fork's dup loop, so refcount is a plain
 * int; SMP atomicity is Stage B's concern.
 */
struct file {
	struct mutex lock;
	enum jnu_file_type type;
	uint64_t offset;
	uint32_t flags;
	int refcount;
	union {
		struct initramfs_file initramfs;
		struct vfs_inode *vfs;
		struct char_device *chardev;
	} u;
};

struct fd_table {
	struct mutex lock;
	struct file *slots[JNU_MAX_FDS];
};

void fd_table_init(struct fd_table *table);

/*
 * Bump refcounts on every populated slot in `src` and copy the slot
 * pointers into `dst`. `dst` must be freshly fd_table_init()ed (all
 * slots NULL) before the call. Used by fork to give the child its own
 * fd table that shares open-file descriptions with the parent.
 */
void fd_table_clone(struct fd_table *dst, struct fd_table *src);

int fd_alloc(struct fd_table *table, struct file *file);
struct file *fd_get(struct fd_table *table, int fd);
struct file *fd_close(struct fd_table *table, int fd);

/* Refcount helpers. file_put destroys the file when refcount hits 0. */
void file_get(struct file *file);
void file_put(struct file *file);

int file_refcount_selftest(void);
