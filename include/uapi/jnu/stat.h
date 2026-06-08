/*
 * include/uapi/jnu/stat.h — Userspace-visible file metadata.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <stdint.h>

struct jnu_stat {
	uint64_t ino;
	uint64_t size;
	uint32_t mode;
	uint32_t type;
};
