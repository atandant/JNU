/*
 * kernel/drivers/fbcon.c — Framebuffer text console.
 *
 * Renders a built-in 8x16 monospace font over Limine's framebuffer.
 * The font table is generated at build time by scripts/gen-font.py
 * into kernel/drivers/font_data.h.
 *
 * Phase-1 console behavior:
 *   - 32-bpp linear framebuffer assumed (BGR or RGB; we write a
 *     single 32-bit color value, the channel order does not matter
 *     for the simple white-on-black palette we use here).
 *   - Single foreground/background pair per call. Optional ANSI-color
 *     pass-through is intentionally NOT implemented in fbcon: the
 *     spec colors panic/err/warn lines on serial; the framebuffer
 *     console renders them in default white.
 *   - Hard scroll: when we cross the bottom row, we memmove the
 *     visible region up one glyph row and clear the new bottom row.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/fbcon.h>
#include <jnu/klog.h>
#include <jnu/string.h>
#include <jnu/types.h>

#include "font_data.h"

#define GLYPH_W		8
#define GLYPH_H		16

#define COLOR_FG	0x00C0C0C0u	/* light gray */
#define COLOR_BG	0x00000000u	/* black */

static struct {
	uint32_t	*pixels;	/* HHDM-mapped */
	uint32_t	width;
	uint32_t	height;
	uint32_t	pitch_px;	/* bytes-per-row / 4 */
	uint32_t	cols;
	uint32_t	rows;
	uint32_t	cur_col;
	uint32_t	cur_row;
	bool		ready;
} fb;

static void put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
	fb.pixels[y * fb.pitch_px + x] = color;
}

static void clear_screen(void)
{
	for (uint32_t y = 0; y < fb.height; y++) {
		uint32_t *row = fb.pixels + y * fb.pitch_px;
		for (uint32_t x = 0; x < fb.width; x++) {
			row[x] = COLOR_BG;
		}
	}
}

static void blit_glyph(uint32_t col, uint32_t row, char c)
{
	uint8_t idx = (uint8_t)c;
	const uint8_t *glyph = font_8x16[idx];

	uint32_t px = col * GLYPH_W;
	uint32_t py = row * GLYPH_H;

	for (uint32_t gy = 0; gy < GLYPH_H; gy++) {
		uint8_t bits = glyph[gy];
		for (uint32_t gx = 0; gx < GLYPH_W; gx++) {
			uint32_t color = (bits & (0x80u >> gx))
				? COLOR_FG : COLOR_BG;
			put_pixel(px + gx, py + gy, color);
		}
	}
}

/*
 * Scroll the visible region up by one glyph row. Done in place via
 * an upward memmove of (rows-1) glyph rows plus a clear of the last
 * one. Pitch may exceed width * 4 (padding bytes on the right of
 * each scanline), so we move per-scanline rather than as one slab.
 */
static void scroll_up(void)
{
	uint32_t shift_rows = GLYPH_H;
	uint32_t keep_rows = fb.height - shift_rows;

	for (uint32_t y = 0; y < keep_rows; y++) {
		uint32_t *dst = fb.pixels + y * fb.pitch_px;
		uint32_t *src = fb.pixels + (y + shift_rows) * fb.pitch_px;
		for (uint32_t x = 0; x < fb.width; x++) {
			dst[x] = src[x];
		}
	}

	for (uint32_t y = keep_rows; y < fb.height; y++) {
		uint32_t *row = fb.pixels + y * fb.pitch_px;
		for (uint32_t x = 0; x < fb.width; x++) {
			row[x] = COLOR_BG;
		}
	}
}

static void newline(void)
{
	fb.cur_col = 0;
	if (fb.cur_row + 1 >= fb.rows) {
		scroll_up();
	} else {
		fb.cur_row++;
	}
}

/*
 * Strip a single ANSI CSI escape starting at *pp. Returns the number
 * of bytes consumed. We accept and silently drop SGR sequences so
 * shared klog text laden with color codes does not litter the screen
 * with garbage.
 */
static size_t skip_ansi(const char *p, size_t left)
{
	if (left < 2 || p[0] != 0x1b || p[1] != '[') {
		return 0;
	}
	size_t i = 2;
	while (i < left) {
		char c = p[i++];
		if (c >= '@' && c <= '~') {
			return i;
		}
	}
	return i;
}

static void emit_char(char c)
{
	switch (c) {
	case '\n':
		newline();
		return;
	case '\r':
		fb.cur_col = 0;
		return;
	case '\t': {
		uint32_t step = 8 - (fb.cur_col % 8);
		for (uint32_t i = 0; i < step; i++) {
			emit_char(' ');
		}
		return;
	}
	case '\b':
		if (fb.cur_col > 0) {
			fb.cur_col--;
			blit_glyph(fb.cur_col, fb.cur_row, ' ');
		}
		return;
	default:
		break;
	}

	if (fb.cur_col >= fb.cols) {
		newline();
	}
	blit_glyph(fb.cur_col, fb.cur_row, c);
	fb.cur_col++;
}

void fbcon_write(const char *buf, size_t len)
{
	if (!fb.ready) {
		return;
	}

	for (size_t i = 0; i < len; ) {
		size_t skip = skip_ansi(buf + i, len - i);
		if (skip) {
			i += skip;
			continue;
		}
		emit_char(buf[i]);
		i++;
	}
}

static struct klog_backend fbcon_backend = {
	.name  = "fbcon",
	.flags = 0,	/* no ANSI on framebuffer */
	.write = fbcon_write,
};

int fbcon_init(const struct fbcon_info *info)
{
	if (!info || !info->addr) {
		return -1;
	}
	if (info->bpp != 32) {
		return -1;
	}

	fb.pixels   = (uint32_t *)info->addr;
	fb.width    = (uint32_t)info->width;
	fb.height   = (uint32_t)info->height;
	fb.pitch_px = (uint32_t)(info->pitch / 4);
	fb.cols     = fb.width  / GLYPH_W;
	fb.rows     = fb.height / GLYPH_H;
	fb.cur_col  = 0;
	fb.cur_row  = 0;
	fb.ready    = true;

	clear_screen();
	klog_register(&fbcon_backend);
	return 0;
}
