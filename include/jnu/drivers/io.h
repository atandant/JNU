/*
 * include/jnu/drivers/io.h — x86 port I/O helpers.
 *
 * Centralizes outb/inb/outw/inw/outl/inl so every driver does not
 * redefine them locally.  These are always-inline because they compile
 * to single instructions and the function-call overhead would be
 * absurd.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/base/types.h>

static inline void outb(uint16_t port, uint8_t val)
{
	__asm__ __volatile__("outb %0, %1" ::"a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
	uint8_t v;
	__asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

static inline void outw(uint16_t port, uint16_t val)
{
	__asm__ __volatile__("outw %0, %1" ::"a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
	uint16_t v;
	__asm__ __volatile__("inw %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

static inline void outl(uint16_t port, uint32_t val)
{
	__asm__ __volatile__("outl %0, %1" ::"a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
	uint32_t v;
	__asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

/*
 * Repeat-string variants for bulk PIO transfers (ATA uses these
 * to move sector-sized buffers in single REP INSW / REP OUTSW
 * bursts).
 */
static inline void insw(uint16_t port, void *buf, size_t count)
{
	__asm__ __volatile__("rep insw"
			     : "+D"(buf), "+c"(count)
			     : "d"(port)
			     : "memory");
}

static inline void outsw(uint16_t port, const void *buf, size_t count)
{
	__asm__ __volatile__("rep outsw" : "+S"(buf), "+c"(count) : "d"(port));
}

/* Short delay (single I/O bus cycle, ~1 µs). Used after PIC/PIT writes. */
static inline void io_wait(void) { outb(0x80, 0); }
