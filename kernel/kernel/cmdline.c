/*
 * kernel/kernel/cmdline.c — Kernel command line parser.
 *
 * Splits a single space-separated string into key / value pairs. The
 * source string is copied into our own bounded buffer so callers do
 * not have to keep the original alive (Limine's strings live in
 * bootloader-reclaimable memory which we may reuse later).
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/cmdline.h>
#include <jnu/string.h>
#include <jnu/types.h>

#define CMDLINE_BUF_SIZE	1024

struct cmdline_entry {
	char	key[CMDLINE_MAX_KEY];
	char	value[CMDLINE_MAX_VALUE];
	bool	used;
};

static char raw[CMDLINE_BUF_SIZE];
static struct cmdline_entry table[CMDLINE_MAX_ENTRIES];
static size_t entry_count;

static bool is_space(char c)
{
	return c == ' ' || c == '\t';
}

static void store_pair(const char *k, size_t klen,
		       const char *v, size_t vlen)
{
	if (entry_count >= CMDLINE_MAX_ENTRIES) {
		return;
	}
	if (klen == 0) {
		return;
	}

	struct cmdline_entry *e = &table[entry_count];

	if (klen >= CMDLINE_MAX_KEY) {
		klen = CMDLINE_MAX_KEY - 1;
	}
	memcpy(e->key, k, klen);
	e->key[klen] = '\0';

	if (vlen >= CMDLINE_MAX_VALUE) {
		vlen = CMDLINE_MAX_VALUE - 1;
	}
	memcpy(e->value, v, vlen);
	e->value[vlen] = '\0';

	e->used = true;
	entry_count++;
}

void cmdline_parse(const char *s)
{
	entry_count = 0;
	memset(table, 0, sizeof(table));

	if (!s) {
		raw[0] = '\0';
		return;
	}

	size_t n = strnlen(s, CMDLINE_BUF_SIZE - 1);
	memcpy(raw, s, n);
	raw[n] = '\0';

	const char *p = raw;
	while (*p) {
		while (is_space(*p)) {
			p++;
		}
		if (!*p) {
			break;
		}

		const char *key = p;
		while (*p && !is_space(*p) && *p != '=') {
			p++;
		}
		size_t klen = (size_t)(p - key);

		const char *value = "1";
		size_t vlen = 1;

		if (*p == '=') {
			p++;
			const char *vstart = p;
			while (*p && !is_space(*p)) {
				p++;
			}
			value = vstart;
			vlen = (size_t)(p - vstart);
		}

		store_pair(key, klen, value, vlen);
	}
}

const char *cmdline_get(const char *key)
{
	for (size_t i = 0; i < entry_count; i++) {
		if (!table[i].used) {
			continue;
		}
		if (strcmp(table[i].key, key) == 0) {
			return table[i].value;
		}
	}
	return NULL;
}

bool cmdline_bool(const char *key)
{
	const char *v = cmdline_get(key);
	if (!v) {
		return false;
	}
	if (v[0] == '0' && v[1] == '\0') {
		return false;
	}
	return true;
}
