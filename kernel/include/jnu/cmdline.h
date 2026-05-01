/*
 * include/jnu/cmdline.h — Kernel command line parser.
 *
 * Limine passes a single string. We split it on spaces into key=value
 * pairs (or bare keys, which map to value "1"). Lookup is O(n) over a
 * small table; v0.0.1 will not have many entries.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>

#define CMDLINE_MAX_ENTRIES 32
#define CMDLINE_MAX_KEY 32
#define CMDLINE_MAX_VALUE 64

/*
 * Parse `s` into the internal table. Idempotent: callable once at boot.
 * Unrecognized keys are stored verbatim; consumers decide whether to
 * complain.
 */
void cmdline_parse(const char *s);

/*
 * Return the value for `key` or NULL if absent. Bare keys (no `=`)
 * resolve to the string "1".
 */
const char *cmdline_get(const char *key);

/* Convenience: return true if `key` is present and not "0". */
bool cmdline_bool(const char *key);
