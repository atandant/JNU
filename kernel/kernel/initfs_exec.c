/*
 * kernel/kernel/initfs_exec.c — Initramfs ELF exec loader and validator.
 *
 * Provides load_initramfs_exec() and validate_initramfs_exec(). These map
 * initramfs files into the generic exec_image abstraction used by the ELF
 * loader.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/base/types.h>
#include <jnu/fs/initramfs.h>
#include <jnu/kernel/elf64.h>
#include <jnu/kernel/exec.h>
#include <jnu/mm/vmm.h>

#include <jnu/kernel/execprot.h>

/*
 * initramfs_exec_read - exec_image read_at callback backed by initramfs.
 */
static ssize_t initramfs_exec_read(void *ctx, uint64_t off, void *buf,
				   size_t len)
{
	return initramfs_read_at(ctx, off, buf, len);
}

int validate_initramfs_exec(const char *path, struct exec_load_info *info)
{
	struct initramfs_file file;
	struct exec_image image;
	int err;

	err = initramfs_lookup(path, &file);
	if (err) {
		return err;
	}

	image.read_at = initramfs_exec_read;
	image.size = file.size;
	image.ctx = &file;

	return elf64_validate_image(&image, info);
}

int load_initramfs_exec(struct addr_space *space, const char *path,
			struct exec_load_info *info, uint64_t *stack)
{
	struct initramfs_file file;
	struct exec_image image;
	int err;

	err = initramfs_lookup(path, &file);
	if (err) {
		return err;
	}

	image.read_at = initramfs_exec_read;
	image.size = file.size;
	image.ctx = &file;

	err = elf64_load_image(space, &image, info);
	if (err) {
		return err;
	}

	return elf64_setup_initial_stack(space, NULL, stack);
}
