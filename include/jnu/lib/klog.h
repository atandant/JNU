/*
 * include/jnu/lib/klog.h — Kernel logging API.
 *
 * `printk` is the workhorse. `pr_*` macros are the ergonomic surface,
 * matching the Linux `pr_*` convention.
 *
 * Backends register through `klog_register`; in v0.0.1 we have two:
 * COM1 serial and the framebuffer console.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/base/compiler.h>
#include <jnu/base/types.h>

enum klog_level {
	KLOG_PANIC = 0,
	KLOG_ERR = 1,
	KLOG_WARN = 2,
	KLOG_INFO = 3,
	KLOG_DEBUG = 4,
};

/*
 * Backend capabilities. ANSI color is opt-in; only backends that set
 * KLOG_BACKEND_ANSI receive escape sequences for KLOG_ERR / KLOG_WARN.
 */
#define KLOG_BACKEND_ANSI (1u << 0)

struct klog_backend {
	const char *name;
	uint32_t flags;
	void (*write)(const char *buf, size_t len);
	struct klog_backend *next;
};

void klog_init(void);
void klog_register(struct klog_backend *be);

void printk(enum klog_level level, const char *fmt, ...) __printf(2, 3);
void vprintk(enum klog_level level, const char *fmt, __builtin_va_list ap);

/*
 * Raw locked write to the ring buffer and backends. Does not format
 * headers or append newlines. Internal kernel use only — must NOT be
 * fed user-controlled bytes (use klog_user_write instead).
 */
void klog_raw_write(enum klog_level level, const char *buf, size_t len);

/*
 * Locked write of userspace stdout/stderr bytes. Each line is prefixed
 * with a non-spoofable "user[pid=N]: " marker so user content cannot
 * impersonate kernel-formatted lines (e.g. forge a "[ssss.uuuuuu] PANIC"
 * entry into the ring buffer that the panic path replays). Control
 * bytes other than '\n' and '\t' are scrubbed to '.' to keep ANSI
 * escapes and other terminal-control sequences out of the COM1 backend.
 */
void klog_user_write(enum klog_level level, int pid, const char *buf,
		     size_t len);

/*
 * Direct-to-backends panic write path. Bypasses ring buffer and locks.
 * Only callable from panic context.
 */
void klog_panic_write(const char *buf, size_t len);

/* Drain the last `n` ring-buffer lines into `cb` (used by panic). */
void klog_drain_tail(size_t n, void (*cb)(const char *line, size_t len));

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#define pr_panic(fmt, ...) printk(KLOG_PANIC, fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...) printk(KLOG_ERR, fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...) printk(KLOG_WARN, fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...) printk(KLOG_INFO, fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...) printk(KLOG_DEBUG, fmt, ##__VA_ARGS__)
#pragma GCC diagnostic pop

/*
 * Formatter primitives, also exported so panic and host tests can use them
 * without going through the ring buffer.
 */
int vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...) __printf(3, 4);
