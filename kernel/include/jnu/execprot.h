/*
 * kernel/kernel/execprot.h — Prototypes for boot-time exec adapters.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/exec.h>
#include <jnu/types.h>

struct addr_space;

int load_initramfs_exec(struct addr_space *space, const char *path,
			struct exec_load_info *info, uint64_t *stack);
int validate_initramfs_exec(const char *path, struct exec_load_info *info);

int validate_vfs_exec(const char *path, struct exec_load_info *info);
