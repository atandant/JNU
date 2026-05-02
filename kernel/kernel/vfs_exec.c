/*
 * kernel/kernel/vfs_exec.c — VFS ELF exec validation adapter.
 *
 * Provides validate_vfs_exec(), which opens a VFS path, wraps it in an
 * exec_image, and runs the ELF64 header/segment validator without mapping
 * anything. Used during boot to confirm a MINIX-backed ELF is loadable.
 *
 * The execve path has its own VFS-backed loader. This file keeps the
 * boot-time validation probe small.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/elf64.h>
#include <jnu/exec.h>
#include <jnu/types.h>
#include <jnu/vfs.h>

#include <jnu/execprot.h>

/*
 * vfs_exec_read - exec_image read_at callback backed by VFS.
 *
 * ctx must be a struct vfs_inode * previously opened by vfs_open().
 * The inode remains open for the lifetime of the exec_image.
 */
static ssize_t vfs_exec_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	return vfs_read(ctx, off, len, buf);
}

/*
 * validate_vfs_exec - Validate the ELF64 at `path` through the VFS.
 *
 * Fills `info` with entry/low/high on success. Does not allocate pages or
 * modify any address space. The VFS inode is opened and closed internally.
 * Returns 0 or negative errno.
 */
int validate_vfs_exec(const char *path, struct exec_load_info *info)
{
	struct vfs_inode *ino;
	struct exec_image image;
	int err;

	err = vfs_open(path, &ino);
	if (err) {
		return err;
	}

	image.read_at = vfs_exec_read;
	image.size = ino->size;
	image.ctx = ino;

	err = elf64_validate_image(&image, info);
	vfs_close(ino);
	return err;
}
