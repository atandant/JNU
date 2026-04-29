/*
 * include/jnu/cpio_newc.h - cpio "newc" archive parser.
 *
 * The parser is deliberately small: it validates one header at a time
 * and reports regular-file/directory metadata to the initramfs layer.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>

#define CPIO_NEWC_MAGIC		"070701"
#define CPIO_NEWC_HEADER_SIZE	110
#define CPIO_NEWC_TRAILER	"TRAILER!!!"

#define CPIO_MODE_TYPE_MASK	0170000u
#define CPIO_MODE_REG		0100000u
#define CPIO_MODE_DIR		0040000u

struct cpio_newc_entry {
	const char	*name;
	size_t		name_len;
	const void	*data;
	size_t		data_len;
	uint32_t	mode;
	size_t		next_off;
};

int cpio_newc_next(const void *archive, size_t archive_len, size_t off,
		   struct cpio_newc_entry *out);
int cpio_newc_selftest(void);
