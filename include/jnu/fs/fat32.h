/*
 * include/jnu/fs/fat32.h — Read-only FAT32 filesystem.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/fs/vfs.h>

extern const struct vfs_ops fat32_ops;

int fat32_selftest(void);
