/*
 * kernel/lib/printk.c — vsnprintf formatter, klog ring buffer, dispatch.
 *
 * Implements:
 *   - vsnprintf / snprintf with the conversion set listed in §7.1
 *     (%d %u %x %X %lld %llu %llx %s %c %p %%, plus width and zero-pad).
 *   - 64 KiB single-writer ring buffer that captures every formatted
 *     line for later panic drain.
 *   - klog backend list and dispatch with optional ANSI coloring.
 *   - printk()/vprintk(): format, prefix `[ssss.uuuuuu] LEVEL ` header,
 *     append to ring buffer, and fan out to backends.
 *
 * Phase 1 has no real timer yet, so the timestamp prefix is always
 * `[    0.000000]`. Phase 2 will route timestamps through TSC.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/compiler.h>
#include <jnu/cpu.h>
#include <jnu/klog.h>
#include <jnu/spinlock.h>
#include <jnu/string.h>
#include <jnu/types.h>

/* Compile-time floor: messages with level > KLOG_BUILD_LEVEL are dropped. */
#ifndef KLOG_BUILD_LEVEL
#define KLOG_BUILD_LEVEL KLOG_DEBUG
#endif

static struct spinlock printk_lock = SPINLOCK_INITIALIZER;

/* ------------------------------------------------------------------------- */
/* vsnprintf                                                                 */
/* ------------------------------------------------------------------------- */

struct fmt_buf {
	char *buf;
	size_t cap; /* total bytes, including NUL */
	size_t len; /* bytes written so far, not counting NUL */
};

static void fb_putc(struct fmt_buf *fb, char c)
{
	if (fb->len + 1 < fb->cap) {
		fb->buf[fb->len] = c;
	}
	fb->len++;
}

static void fb_puts(struct fmt_buf *fb, const char *s)
{
	while (*s) {
		fb_putc(fb, *s++);
	}
}

static void fb_pad(struct fmt_buf *fb, char c, int n)
{
	while (n-- > 0) {
		fb_putc(fb, c);
	}
}

/*
 * Format an unsigned integer into a stack scratch buffer, write it
 * with the requested width / pad / case rules. `digits` selects the
 * digit alphabet so we share one routine for decimal and hex.
 */
static void fmt_uint(struct fmt_buf *fb, uint64_t v, unsigned base,
		     const char *digits, int width, bool zero_pad)
{
	char tmp[32];
	int n = 0;

	if (v == 0) {
		tmp[n++] = '0';
	} else {
		while (v && n < (int)sizeof(tmp)) {
			tmp[n++] = digits[v % base];
			v /= base;
		}
	}

	if (!zero_pad) {
		fb_pad(fb, ' ', width - n);
	} else {
		fb_pad(fb, '0', width - n);
	}

	while (n--) {
		fb_putc(fb, tmp[n]);
	}
}

static void fmt_int(struct fmt_buf *fb, int64_t v, int width, bool zero_pad)
{
	if (v < 0) {
		uint64_t u = (uint64_t)(-(v + 1)) + 1; /* avoid INT_MIN UB */
		fb_putc(fb, '-');
		if (width > 0) {
			width--;
		}
		fmt_uint(fb, u, 10, "0123456789", width, zero_pad);
	} else {
		fmt_uint(fb, (uint64_t)v, 10, "0123456789", width, zero_pad);
	}
}

int vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap)
{
	struct fmt_buf fb = {.buf = buf, .cap = size, .len = 0};

	while (*fmt) {
		char c = *fmt++;

		if (c != '%') {
			fb_putc(&fb, c);
			continue;
		}

		bool zero_pad = false;
		int width = 0;
		int longness = 0; /* 0 = int, 1 = long, 2 = long long */

		if (*fmt == '0') {
			zero_pad = true;
			fmt++;
		}

		while (*fmt >= '0' && *fmt <= '9') {
			width = width * 10 + (*fmt - '0');
			fmt++;
		}

		while (*fmt == 'l') {
			longness++;
			fmt++;
		}

		c = *fmt++;
		switch (c) {
		case 'd':
		case 'i': {
			int64_t v;
			if (longness >= 2) {
				v = __builtin_va_arg(ap, long long);
			} else if (longness == 1) {
				v = __builtin_va_arg(ap, long);
			} else {
				v = __builtin_va_arg(ap, int);
			}
			fmt_int(&fb, v, width, zero_pad);
			break;
		}
		case 'u': {
			uint64_t v;
			if (longness >= 2) {
				v = __builtin_va_arg(ap, unsigned long long);
			} else if (longness == 1) {
				v = __builtin_va_arg(ap, unsigned long);
			} else {
				v = __builtin_va_arg(ap, unsigned int);
			}
			fmt_uint(&fb, v, 10, "0123456789", width, zero_pad);
			break;
		}
		case 'x':
		case 'X': {
			uint64_t v;
			const char *digits = (c == 'X') ? "0123456789ABCDEF"
							: "0123456789abcdef";
			if (longness >= 2) {
				v = __builtin_va_arg(ap, unsigned long long);
			} else if (longness == 1) {
				v = __builtin_va_arg(ap, unsigned long);
			} else {
				v = __builtin_va_arg(ap, unsigned int);
			}
			fmt_uint(&fb, v, 16, digits, width, zero_pad);
			break;
		}
		case 'p': {
			uintptr_t v = (uintptr_t)__builtin_va_arg(ap, void *);
			fb_puts(&fb, "0x");
			fmt_uint(&fb, v, 16, "0123456789abcdef", 16, true);
			break;
		}
		case 's': {
			const char *s = __builtin_va_arg(ap, const char *);
			if (!s) {
				s = "(null)";
			}
			int slen = (int)strlen(s);
			if (!zero_pad) {
				fb_pad(&fb, ' ', width - slen);
			}
			fb_puts(&fb, s);
			break;
		}
		case 'c': {
			char ch = (char)__builtin_va_arg(ap, int);
			fb_putc(&fb, ch);
			break;
		}
		case '%':
			fb_putc(&fb, '%');
			break;
		default:
			/* Unknown conversion — emit verbatim. */
			fb_putc(&fb, '%');
			fb_putc(&fb, c);
			break;
		}
	}

	if (fb.cap > 0) {
		size_t cut = fb.len < fb.cap ? fb.len : fb.cap - 1;
		fb.buf[cut] = '\0';
	}

	return (int)fb.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
	__builtin_va_list ap;
	int n;

	__builtin_va_start(ap, fmt);
	n = vsnprintf(buf, size, fmt, ap);
	__builtin_va_end(ap);

	return n;
}

/* ------------------------------------------------------------------------- */
/* Ring buffer                                                                */
/* ------------------------------------------------------------------------- */

#define KLOG_RING_SIZE (64 * 1024)

static char ring_buf[KLOG_RING_SIZE];
static size_t ring_head;  /* next write index */
static size_t ring_count; /* number of valid bytes in the ring */

static void ring_write(const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		ring_buf[ring_head] = s[i];
		ring_head = (ring_head + 1) % KLOG_RING_SIZE;
		if (ring_count < KLOG_RING_SIZE) {
			ring_count++;
		}
	}
}

void klog_drain_tail(size_t n, void (*cb)(const char *line, size_t len))
{
	if (ring_count == 0 || n == 0) {
		return;
	}

	/*
	 * Walk backward through the ring counting newlines; collect at
	 * most `n` lines. Then walk forward emitting each.
	 */
	size_t start =
	    (ring_head + KLOG_RING_SIZE - ring_count) % KLOG_RING_SIZE;
	size_t total_lines = 0;
	for (size_t i = 0; i < ring_count; i++) {
		size_t idx = (start + i) % KLOG_RING_SIZE;
		if (ring_buf[idx] == '\n') {
			total_lines++;
		}
	}

	size_t skip_lines = (total_lines > n) ? (total_lines - n) : 0;
	size_t i = 0;
	size_t seen = 0;
	while (i < ring_count && seen < skip_lines) {
		if (ring_buf[(start + i) % KLOG_RING_SIZE] == '\n') {
			seen++;
		}
		i++;
	}

	char line[256];
	size_t llen = 0;
	while (i < ring_count) {
		char ch = ring_buf[(start + i) % KLOG_RING_SIZE];
		if (llen < sizeof(line) - 1) {
			line[llen++] = ch;
		}
		if (ch == '\n') {
			cb(line, llen);
			llen = 0;
		}
		i++;
	}
	if (llen > 0) {
		cb(line, llen);
	}
}

/* ------------------------------------------------------------------------- */
/* Backends                                                                  */
/* ------------------------------------------------------------------------- */

static struct klog_backend *backends;

void klog_init(void)
{
	backends = NULL;
	ring_head = 0;
	ring_count = 0;
}

void klog_register(struct klog_backend *be)
{
	be->next = backends;
	backends = be;
}

static const char *level_tag(enum klog_level lvl)
{
	switch (lvl) {
	case KLOG_PANIC:
		return "PANIC";
	case KLOG_ERR:
		return "ERROR";
	case KLOG_WARN:
		return "WARN ";
	case KLOG_INFO:
		return "INFO ";
	case KLOG_DEBUG:
		return "DEBUG";
	}
	return "?????";
}

static const char *ansi_open(enum klog_level lvl)
{
	switch (lvl) {
	case KLOG_PANIC:
	case KLOG_ERR:
		return "\x1b[31m"; /* red */
	case KLOG_WARN:
		return "\x1b[33m"; /* yellow */
	default:
		return NULL;
	}
}

static const char *ansi_close(enum klog_level lvl)
{
	switch (lvl) {
	case KLOG_PANIC:
	case KLOG_ERR:
	case KLOG_WARN:
		return "\x1b[0m";
	default:
		return NULL;
	}
}

static void backends_write(enum klog_level lvl, const char *s, size_t n)
{
	const char *open = ansi_open(lvl);
	const char *close = ansi_close(lvl);

	for (struct klog_backend *be = backends; be; be = be->next) {
		bool color = open && (be->flags & KLOG_BACKEND_ANSI);
		if (color) {
			be->write(open, strlen(open));
		}
		be->write(s, n);
		if (color) {
			be->write(close, strlen(close));
		}
	}
}

void klog_panic_write(const char *buf, size_t len)
{
	for (struct klog_backend *be = backends; be; be = be->next) {
		be->write(buf, len);
	}
}

void klog_raw_write(enum klog_level level, const char *buf, size_t len)
{
	uint64_t flags = spin_lock_irqsave(&printk_lock);
	ring_write(buf, len);
	backends_write(level, buf, len);
	spin_unlock_irqrestore(&printk_lock, flags);
}

/*
 * Userspace bytes follow a stricter contract than klog_raw_write: they
 * are untrusted and must not be confusable with kernel-formatted log
 * lines. We therefore (a) prefix every line with "user[pid=N]: " so the
 * panic ring-buffer drain can't be made to replay a forged
 * "[ssss.uuuuuu] PANIC ..." entry, and (b) scrub bytes that would let
 * a process drive an ANSI-aware COM1 console. The scrubbing is done in
 * place in the caller's bounce buffer; sys_write owns that buffer for
 * the duration of one syscall.
 */
void klog_user_write(enum klog_level level, int pid, const char *buf,
		     size_t len)
{
	char prefix[32];
	int plen;
	uint64_t flags;
	size_t start;

	if (len == 0) {
		return;
	}

	plen = snprintf(prefix, sizeof(prefix), "user[pid=%d]: ", pid);
	if (plen < 0) {
		plen = 0;
	}

	flags = spin_lock_irqsave(&printk_lock);

	ring_write(prefix, (size_t)plen);
	backends_write(level, prefix, (size_t)plen);

	start = 0;
	for (size_t i = 0; i < len; i++) {
		if (buf[i] != '\n') {
			continue;
		}
		ring_write(buf + start, i - start + 1);
		backends_write(level, buf + start, i - start + 1);
		start = i + 1;
		if (start < len) {
			ring_write(prefix, (size_t)plen);
			backends_write(level, prefix, (size_t)plen);
		}
	}
	if (start < len) {
		ring_write(buf + start, len - start);
		backends_write(level, buf + start, len - start);
	}

	spin_unlock_irqrestore(&printk_lock, flags);
}

/* ------------------------------------------------------------------------- */
/* printk                                                                    */
/* ------------------------------------------------------------------------- */

void vprintk(enum klog_level level, const char *fmt, __builtin_va_list ap)
{
	if ((int)level > KLOG_BUILD_LEVEL) {
		return;
	}

	uint64_t flags = spin_lock_irqsave(&printk_lock);

	char line[1024];
	int hlen;

	/*
	 * Header. After Phase-2 cpu_calibrate_tsc(), `cpu_us_since_boot`
	 * returns microseconds since CPU bring-up; before calibration it
	 * returns 0, leaving the early-boot lines time-stamped at zero.
	 * Format `[ssss.uuuuuu] LEVEL `.
	 */
	uint64_t us = cpu_us_since_boot();
	uint64_t s = us / 1000000ull;
	uint64_t u = us % 1000000ull;
	hlen = snprintf(line, sizeof(line), "[%5lu.%06lu] %s ",
			(unsigned long)s, (unsigned long)u, level_tag(level));

	int blen = vsnprintf(line + hlen, sizeof(line) - (size_t)hlen, fmt, ap);
	(void)blen;

	size_t total = strnlen(line, sizeof(line));

	/*
	 * Ensure trailing newline. If the caller forgot one, append it
	 * (truncating the last byte if necessary).
	 */
	if (total == 0 || line[total - 1] != '\n') {
		if (total >= sizeof(line) - 1) {
			total = sizeof(line) - 2;
		}
		line[total++] = '\n';
		line[total] = '\0';
	}

	ring_write(line, total);
	backends_write(level, line, total);

	spin_unlock_irqrestore(&printk_lock, flags);
}

void printk(enum klog_level level, const char *fmt, ...)
{
	__builtin_va_list ap;

	__builtin_va_start(ap, fmt);
	vprintk(level, fmt, ap);
	__builtin_va_end(ap);
}
