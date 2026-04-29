/*
 * include/jnu/symbols.h — Symbol-table lookup for backtrace and panic.
 *
 * The table itself is generated from the linked kernel ELF by
 * `scripts/gen-symbols.sh`, compiled into `kernel/kernel/symbols.c`.
 * Lookups are O(log n) via binary search.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>

struct ksymbol {
	uint64_t	addr;
	const char	*name;
};

extern const struct ksymbol jnu_symbols[];
extern const size_t jnu_symbols_count;

/*
 * Look up the symbol covering `addr`. On success, `*name` is set to a
 * pointer into a static string table and `*offset` to addr − sym.addr.
 * Returns true on hit, false otherwise.
 */
bool symbols_lookup(uint64_t addr, const char **name, uint64_t *offset);
