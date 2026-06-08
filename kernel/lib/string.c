/*
 * kernel/lib/string.c — Freestanding memory and string primitives.
 *
 * Hand-rolled byte-at-a-time implementations. The compiler may emit
 * implicit calls to memcpy/memset for struct copies and zero-init,
 * so these names must exist with C-standard prototypes regardless of
 * whether the kernel calls them explicitly.
 *
 * No SSE; built with -mgeneral-regs-only. Performance is irrelevant
 * in v0.0.1; correctness is not.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/lib/string.h>

void *memcpy(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;

	while (n--) {
		*d++ = *s++;
	}

	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;

	if (d == s || n == 0) {
		return dst;
	}

	if (d < s) {
		while (n--) {
			*d++ = *s++;
		}
	} else {
		d += n;
		s += n;
		while (n--) {
			*--d = *--s;
		}
	}

	return dst;
}

void *memset(void *dst, int c, size_t n)
{
	unsigned char *d = dst;
	unsigned char v = (unsigned char)c;

	while (n--) {
		*d++ = v;
	}

	return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p = a;
	const unsigned char *q = b;

	while (n--) {
		if (*p != *q) {
			return (int)*p - (int)*q;
		}
		p++;
		q++;
	}

	return 0;
}

size_t strlen(const char *s)
{
	const char *p = s;

	while (*p) {
		p++;
	}

	return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t maxlen)
{
	size_t i;

	for (i = 0; i < maxlen && s[i]; i++) {
		;
	}

	return i;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}

	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
	while (n && *a && *a == *b) {
		a++;
		b++;
		n--;
	}

	if (n == 0) {
		return 0;
	}

	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strchr(const char *s, int c)
{
	char ch = (char)c;

	for (;;) {
		if (*s == ch) {
			return (char *)s;
		}
		if (*s == '\0') {
			return NULL;
		}
		s++;
	}
}
