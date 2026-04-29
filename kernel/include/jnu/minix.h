/*
 * include/jnu/minix.h — MINIX v1 filesystem.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/vfs.h>

extern const struct vfs_ops minix_ops;

int minix_selftest(void);
