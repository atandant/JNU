/*
 * include/jnu/minix.h — MINIX v1 filesystem.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/vfs.h>

extern const struct vfs_ops minix_ops;

int bufcache_selftest(void);
void bufcache_log_stats(void);
int minix_bitmap_selftest(void);
int minix_dir_selftest(void);
int minix_fsync_selftest(void);
int minix_selftest(void);
int minix_write_selftest(void);
