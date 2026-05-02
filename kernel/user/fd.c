/*
 * kernel/user/fd.c - File descriptor table and `struct file` lifetime.
 *
 * Refcounting contract (v0.0.2.1):
 *
 *   1. Whoever allocates a `struct file` initializes refcount = 1
 *      *before* installing the file into an fd table. That initial
 *      reference is owned by the slot, not by the allocator.
 *   2. Sharing a file across slots (e.g. fork's fd-table dup) calls
 *      `file_get` to bump refcount. Forgetting this causes premature
 *      destroy on first close.
 *   3. Closing a slot calls `file_put`, which decrements refcount and
 *      destroys when it reaches zero. No caller in the kernel frees a
 *      `struct file` directly.
 *
 * `file_destroy` is therefore static to this file. The previous
 * `file_destroy(fd_close(...))` idiom is replaced with
 * `file_put(fd_close(...))` everywhere.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/fd.h>
#include <jnu/kmalloc.h>
#include <jnu/string.h>

static void file_destroy(struct file *file)
{
	if (!file) {
		return;
	}
	if (file->type == JNU_FILE_VFS) {
		vfs_close(file->u.vfs);
	}
	/*
	 * JNU_FILE_CHARDEV: the char_device is a singleton owned by the
	 * driver (e.g. kbd_get_chardev() returns &kbd_cdev). Nothing to
	 * release here besides the surrounding `struct file` slab entry.
	 */
	kfree(file);
}

void fd_table_init(struct fd_table *table) { memset(table, 0, sizeof(*table)); }

void fd_table_clone(struct fd_table *dst, struct fd_table *src)
{
	if (!dst || !src) {
		return;
	}
	for (int fd = 0; fd < JNU_MAX_FDS; fd++) {
		struct file *f = src->slots[fd];

		if (!f) {
			dst->slots[fd] = NULL;
			continue;
		}
		file_get(f);
		dst->slots[fd] = f;
	}
}

int fd_alloc(struct fd_table *table, struct file *file)
{
	if (!table || !file) {
		return -EINVAL;
	}

	for (int fd = 3; fd < JNU_MAX_FDS; fd++) {
		if (!table->slots[fd]) {
			table->slots[fd] = file;
			return fd;
		}
	}

	return -EMFILE;
}

struct file *fd_get(struct fd_table *table, int fd)
{
	if (!table || fd < 0 || fd >= JNU_MAX_FDS) {
		return NULL;
	}
	return table->slots[fd];
}

struct file *fd_close(struct fd_table *table, int fd)
{
	struct file *file;

	if (!table || fd < 0 || fd >= JNU_MAX_FDS) {
		return NULL;
	}

	file = table->slots[fd];
	table->slots[fd] = NULL;
	return file;
}

void file_get(struct file *file)
{
	if (!file) {
		return;
	}
	file->refcount++;
}

void file_put(struct file *file)
{
	if (!file) {
		return;
	}
	if (--file->refcount > 0) {
		return;
	}
	file_destroy(file);
}

/* ------------------------------------------------------------------------- */
/* Selftest                                                                   */
/* ------------------------------------------------------------------------- */

int file_refcount_selftest(void)
{
	struct file *f;
	struct fd_table table_a;
	struct fd_table table_b;
	int fd_a;

	f = kzalloc(sizeof(*f));
	if (!f) {
		return -ENOMEM;
	}
	f->type = JNU_FILE_CHARDEV; /* destroy is a no-op besides kfree */
	f->u.chardev = NULL;
	f->refcount = 1;

	fd_table_init(&table_a);
	fd_table_init(&table_b);

	fd_a = fd_alloc(&table_a, f);
	if (fd_a < 0) {
		file_put(f);
		return fd_a;
	}

	/* Simulate a fork-style dup: fd_table_clone bumps the refcount. */
	fd_table_clone(&table_b, &table_a);
	if (f->refcount != 2) {
		(void)fd_close(&table_a, fd_a);
		(void)fd_close(&table_b, fd_a);
		file_put(f);
		return -EINVAL;
	}

	/* Drop the parent slot. The child still holds a reference. */
	{
		struct file *removed = fd_close(&table_a, fd_a);

		if (removed != f) {
			return -EINVAL;
		}
		file_put(removed);
	}
	if (f->refcount != 1) {
		return -EINVAL;
	}

	/*
	 * Drop the last reference. After this, `f` is freed; touching it
	 * is a use-after-free, so we only verify by exit.
	 */
	{
		struct file *removed = fd_close(&table_b, fd_a);

		if (removed != f) {
			return -EINVAL;
		}
		file_put(removed);
	}

	return 0;
}
