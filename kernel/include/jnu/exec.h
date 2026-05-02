/*
 * include/jnu/exec.h - Executable image abstraction.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>

struct exec_image {
	ssize_t (*read_at)(void *ctx, uint64_t off, void *buf, size_t len);
	uint64_t size;
	void *ctx;
};

struct exec_load_info {
	uint64_t entry;
	uint64_t low;
	uint64_t high;
};

struct addr_space;
struct vfs_inode;

int elf64_validate_image(const struct exec_image *image,
			 struct exec_load_info *info);
int elf64_load_image(struct addr_space *space, const struct exec_image *image,
		     struct exec_load_info *info);
int elf64_setup_initial_stack(struct addr_space *space, uint64_t *stack_out);
