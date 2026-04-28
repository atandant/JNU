/*
 * kernel/kernel/panic.c — Phase-1 minimal panic.
 *
 * Format a headline of the form
 *
 *     [ssss.uuuuuu] PANIC: <message>
 *     System halted.
 *
 * write it to all registered klog backends bypassing the ring buffer
 * and any locks, then `cli; hlt; jmp $`. Phase 2 replaces this with
 * the full canonical panic in §13.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/compiler.h>
#include <jnu/klog.h>
#include <jnu/panic.h>
#include <jnu/string.h>

__noreturn void panic(const char *fmt, ...)
{
	__asm__ __volatile__ ("cli");

	char line[512];
	int n;

	n = snprintf(line, sizeof(line), "[%5u.%06u] PANIC: ", 0u, 0u);

	__builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	int m = vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
	__builtin_va_end(ap);
	(void)m;

	size_t total = strnlen(line, sizeof(line));
	if (total == 0 || line[total - 1] != '\n') {
		if (total >= sizeof(line) - 1) {
			total = sizeof(line) - 2;
		}
		line[total++] = '\n';
		line[total] = '\0';
	}

	klog_panic_write(line, total);

	static const char halted[] = "System halted.\n";
	klog_panic_write(halted, sizeof(halted) - 1);

	for (;;) {
		__asm__ __volatile__ ("cli; hlt");
	}
}
