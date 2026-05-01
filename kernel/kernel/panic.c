/*
 * kernel/kernel/panic.c — Canonical kernel panic, register dump, backtrace.
 *
 * Phase-2 implementation. Output follows §13 exactly: headline,
 * exception decode (when called from exception path), faulting address,
 * CPU/ring/task line, RIP with symbol, all 16 GPRs in aligned columns,
 * control registers, frame-pointer backtrace with symbol resolution,
 * last 32 ring-buffer lines, `System halted.`
 *
 * Panic-mode print path: IF cleared, no locks taken, no allocation, no
 * ring-buffer queueing — direct write to backends.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/compiler.h>
#include <jnu/idt.h>
#include <jnu/klog.h>
#include <jnu/paging.h>
#include <jnu/panic.h>
#include <jnu/sched.h>
#include <jnu/string.h>
#include <jnu/symbols.h>
#include <jnu/types.h>

static void emit_raw(const char *s, size_t n) { klog_panic_write(s, n); }

static void emit_str(const char *s) { emit_raw(s, strlen(s)); }

__printf(1, 2) static void emit_fmt(const char *fmt, ...)
{
	char buf[512];
	__builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	__builtin_va_end(ap);
	if (n < 0) {
		return;
	}
	emit_raw(buf, (size_t)n);
}

static void emit_tail_cb(const char *line, size_t len)
{
	emit_raw("  ", 2);
	emit_raw(line, len);
}

/* ------------------------------------------------------------------------- */
/* Register helpers                                                           */
/* ------------------------------------------------------------------------- */

static uint64_t read_cr0(void)
{
	uint64_t v;
	__asm__ __volatile__("mov %%cr0, %0" : "=r"(v));
	return v;
}
static uint64_t read_cr3(void)
{
	uint64_t v;
	__asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
	return v;
}
static uint64_t read_cr4(void)
{
	uint64_t v;
	__asm__ __volatile__("mov %%cr4, %0" : "=r"(v));
	return v;
}

/* Decode a #PF error code into a human string. */
static const char *pf_decode(uint64_t err, char *buf, size_t cap)
{
	const char *kind = (err & 0x2) ? "write" : "read";
	const char *pres = (err & 0x1) ? "protection" : "not-present";
	const char *priv = (err & 0x4) ? "user" : "supervisor";
	const char *fetch = (err & 0x10) ? ", fetch" : "";
	snprintf(buf, cap, "(%s, %s, %s%s)", kind, pres, priv, fetch);
	return buf;
}

static const char *vector_kind(uint64_t v)
{
	switch (v) {
	case 0:
		return "#DE";
	case 1:
		return "#DB";
	case 2:
		return "#NMI";
	case 3:
		return "#BP";
	case 6:
		return "#UD";
	case 8:
		return "#DF";
	case 13:
		return "#GP";
	case 14:
		return "#PF";
	case 18:
		return "#MC";
	default:
		return "exception";
	}
}

/* ------------------------------------------------------------------------- */
/* Backtrace                                                                  */
/* ------------------------------------------------------------------------- */

static void emit_symbol(uint64_t rip)
{
	const char *name;
	uint64_t off;
	if (symbols_lookup(rip, &name, &off)) {
		emit_fmt("%s+0x%lx", name, (unsigned long)off);
	} else {
		emit_str("(?)");
	}
}

static bool plausible_kernel_addr(uint64_t a)
{
	return a >= 0xFFFF800000000000ull;
}

static void emit_backtrace(uint64_t rbp)
{
	emit_str("Backtrace:\n");
	int depth = 0;
	while (rbp && plausible_kernel_addr(rbp) && depth < 32) {
		uint64_t *frame = (uint64_t *)(uintptr_t)rbp;
		uint64_t saved_rbp = frame[0];
		uint64_t ret_rip = frame[1];
		if (!plausible_kernel_addr(ret_rip)) {
			break;
		}
		emit_fmt("  #%d  0x%016lx   ", depth, (unsigned long)ret_rip);
		emit_symbol(ret_rip);
		emit_str("\n");
		if (saved_rbp <= rbp) {
			break; /* refuse to go backward — corrupt frame */
		}
		rbp = saved_rbp;
		depth++;
	}
}

/* ------------------------------------------------------------------------- */
/* Headers and bodies                                                         */
/* ------------------------------------------------------------------------- */

static void emit_regs(const struct cpu_state *st, uint64_t cr2)
{
	struct task *task = sched_current();

	if (task) {
		emit_fmt("CPU 0  ring %u  pid=%d tid=%d task=%s\n\n",
			 (unsigned)(st->cs & 3), task->pid, task->tid,
			 task->name ? task->name : "(unnamed)");
	} else {
		emit_fmt("CPU 0  ring %u  task=<none>\n\n",
			 (unsigned)(st->cs & 3));
	}

	emit_fmt("RIP=0x%016lx   ", (unsigned long)st->rip);
	emit_symbol(st->rip);
	emit_str("\n");
	emit_fmt("CS =0x%04lx  SS =0x%04lx   RFLAGS=0x%016lx\n",
		 (unsigned long)st->cs, (unsigned long)st->ss,
		 (unsigned long)st->rflags);
	emit_fmt("RSP=0x%016lx\n\n", (unsigned long)st->rsp);

	emit_fmt("RAX=0x%016lx   RBX=0x%016lx\n", (unsigned long)st->rax,
		 (unsigned long)st->rbx);
	emit_fmt("RCX=0x%016lx   RDX=0x%016lx\n", (unsigned long)st->rcx,
		 (unsigned long)st->rdx);
	emit_fmt("RSI=0x%016lx   RDI=0x%016lx\n", (unsigned long)st->rsi,
		 (unsigned long)st->rdi);
	emit_fmt("RBP=0x%016lx   R8 =0x%016lx\n", (unsigned long)st->rbp,
		 (unsigned long)st->r8);
	emit_fmt("R9 =0x%016lx   R10=0x%016lx\n", (unsigned long)st->r9,
		 (unsigned long)st->r10);
	emit_fmt("R11=0x%016lx   R12=0x%016lx\n", (unsigned long)st->r11,
		 (unsigned long)st->r12);
	emit_fmt("R13=0x%016lx   R14=0x%016lx\n", (unsigned long)st->r13,
		 (unsigned long)st->r14);
	emit_fmt("R15=0x%016lx\n\n", (unsigned long)st->r15);

	emit_fmt("CR0=0x%016lx   CR2=0x%016lx\n", (unsigned long)read_cr0(),
		 (unsigned long)cr2);
	emit_fmt("CR3=0x%016lx   CR4=0x%016lx\n\n", (unsigned long)read_cr3(),
		 (unsigned long)read_cr4());
}

/* ------------------------------------------------------------------------- */
/* Public panic API                                                           */
/* ------------------------------------------------------------------------- */

__noreturn void panic(const char *fmt, ...)
{
	__asm__ __volatile__("cli");

	emit_str("PANIC: ");
	char body[256];
	__builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	int n = vsnprintf(body, sizeof(body), fmt, ap);
	__builtin_va_end(ap);
	if (n < 0) {
		n = 0;
	}
	emit_raw(body, (size_t)n);
	if (n == 0 || body[n - 1] != '\n') {
		emit_str("\n");
	}

	uint64_t rbp;
	__asm__ __volatile__("mov %%rbp, %0" : "=r"(rbp));
	emit_str("\n");
	emit_backtrace(rbp);

	emit_str("\nLast log lines:\n");
	klog_drain_tail(32, emit_tail_cb);

	emit_str("\nThe system cannot progress further...\n");
	emit_str("\nSystem halted.\n");

	for (;;) {
		__asm__ __volatile__("cli; hlt");
	}
}

__noreturn void panic_with_state(struct cpu_state *st)
{
	__asm__ __volatile__("cli");

	emit_str("PANIC ");
	emit_str(vector_kind(st->vector));
	emit_str("\n\n");

	uint64_t cr2 = paging_read_cr2();

	emit_fmt("Exception: %s (vector %u)  error=0x%04lx",
		 vector_kind(st->vector), (unsigned)st->vector,
		 (unsigned long)st->error_code);

	if (st->vector == 14) {
		char dec[64];
		emit_str("  ");
		emit_str(pf_decode(st->error_code, dec, sizeof(dec)));
	}
	emit_str("\n");

	if (st->vector == 14) {
		emit_fmt("Faulting address: 0x%016lx\n", (unsigned long)cr2);
	}
	emit_str("\n");

	emit_regs(st, cr2);

	emit_backtrace(st->rbp);

	emit_str("\nLast log lines:\n");
	klog_drain_tail(32, emit_tail_cb);

	emit_str("\nSystem halted.\n");

	for (;;) {
		__asm__ __volatile__("cli; hlt");
	}
}
