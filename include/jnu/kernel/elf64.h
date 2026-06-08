/*
 * include/jnu/kernel/elf64.h - ELF64 static executable loader.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/kernel/exec.h>

int elf64_validate_image(const struct exec_image *image,
			 struct exec_load_info *info);
int elf64_load_image(struct addr_space *space, const struct exec_image *image,
		     struct exec_load_info *info);
int elf64_setup_initial_stack(struct addr_space *space,
			      const struct exec_strings *strings,
			      uint64_t *stack_out);
int elf64_selftest(void);
