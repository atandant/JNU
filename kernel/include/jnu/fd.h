/*
 * include/jnu/fd.h - File descriptor table scaffolding.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>
#include <jnu/chardev.h>
#include <jnu/initramfs.h>
#include <jnu/vfs.h>

#define JNU_MAX_FDS	32

enum jnu_file_type {
	JNU_FILE_INITRAMFS,
	JNU_FILE_VFS,
	JNU_FILE_CHARDEV,
};

struct jnu_stat {
	uint64_t	ino;
	uint64_t	size;
	uint32_t	mode;
	uint32_t	type;
};

struct file {
	enum jnu_file_type	type;
	uint64_t		offset;
	uint32_t		flags;
	union {
		struct initramfs_file initramfs;
		struct vfs_inode *vfs;
		struct char_device *chardev;
	} u;
};

struct fd_table {
	struct file	*slots[JNU_MAX_FDS];
};

void fd_table_init(struct fd_table *table);
int fd_alloc(struct fd_table *table, struct file *file);
struct file *fd_get(struct fd_table *table, int fd);
struct file *fd_close(struct fd_table *table, int fd);
void file_destroy(struct file *file);
