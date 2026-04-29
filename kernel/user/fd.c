/*
 * kernel/user/fd.c - File descriptor table scaffolding.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/fd.h>
#include <jnu/errno.h>
#include <jnu/kmalloc.h>
#include <jnu/string.h>

void fd_table_init(struct fd_table *table)
{
	memset(table, 0, sizeof(*table));
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

void file_destroy(struct file *file)
{
	if (!file) {
		return;
	}
	if (file->type == JNU_FILE_VFS) {
		vfs_close(file->u.vfs);
	}
	kfree(file);
}
