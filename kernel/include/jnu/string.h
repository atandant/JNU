/*
 * include/jnu/string.h — Freestanding memory and string primitives.
 *
 * Hand-rolled, no SSE. The compiler may still emit calls to memcpy /
 * memset for struct copies and large initializers, so these symbols
 * must exist with the standard signatures.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);
