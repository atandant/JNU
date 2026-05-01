/*
 * include/jnu/initramfs.h - Read-only initramfs lookup.
 *
 * v0.0.2 uses a Limine-loaded cpio newc archive as the mandatory
 * bootstrap filesystem for /init and early userspace test programs.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>

struct initramfs_file {
	const char *name;
	size_t name_len;
	const void *data;
	size_t size;
	uint32_t mode;
};

int initramfs_init(void *base, size_t len);
bool initramfs_ready(void);
int initramfs_lookup(const char *path, struct initramfs_file *out);
ssize_t initramfs_read_at(const struct initramfs_file *file, uint64_t off,
			  void *buf, size_t len);
int initramfs_selftest(void);
