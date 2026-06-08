/*
 * kernel/arch/x86_64/fpu.c — Eager FPU/SSE state save/restore.
 *
 * v0.0.3 §2.7: XSAVE or FXSAVE on every context switch.S
 *
 * Boot-time detection:
 *   - CPUID.01H:ECX.XSAVE (bit 26) → use XSAVE/XRSTOR.
 *   - Otherwise → use FXSAVE/FXRSTOR.
 *   - Either path requires CPUID.01H:EDX.FXSR (bit 24); absence panics.
 *
 * CR0/CR4 setup:
 *   CR0.MP=1, CR0.EM=0, CR0.NE=1, CR0.TS=0 (stays 0 forever).
 *   CR4.OSFXSR=1, CR4.OSXMMEXCPT=1.
 *   CR4.OSXSAVE=1 if XSAVE supported.
 *   XCR0 = X87 | SSE = 0x3 if XSAVE supported.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/arch/cpu.h>
#include <jnu/base/types.h>
#include <jnu/kernel/panic.h>
#include <jnu/kernel/sched.h>
#include <jnu/lib/klog.h>
#include <jnu/lib/string.h>

/* Use-xsave flag cached at boot so save/restore are branchless-ish. */
static bool use_xsave;

/*
 * Canonical initial FPU state.  Copied into every new task's buffer.
 * 64-byte aligned per FXSAVE/XSAVE requirements.
 *
 * The layout is FXSAVE format (Intel SDM Vol. 1 §10.5.1):
 *   bytes  0–1: FCW  (x87 control word)
 *   bytes 24–27: MXCSR
 *   bytes 28–31: MXCSR_MASK
 *
 * For XSAVE the legacy region (bytes 0–511) has the same layout.
 * We initialize to the FNINIT-equivalent state with MXCSR = 0x1F80
 * (all exceptions masked).
 */
_Alignas(64) static uint8_t fpu_init_state_buf[1024];

static void cpuid_leaf(uint32_t leaf, uint32_t subleaf, uint32_t *a,
		       uint32_t *b, uint32_t *c, uint32_t *d)
{
	__asm__ __volatile__("cpuid"
			     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
			     : "a"(leaf), "c"(subleaf));
}

static uint64_t read_cr0(void)
{
	uint64_t v;
	__asm__ __volatile__("mov %%cr0, %0" : "=r"(v));
	return v;
}

static void write_cr0(uint64_t v)
{
	__asm__ __volatile__("mov %0, %%cr0" ::"r"(v) : "memory");
}

static uint64_t read_cr4(void)
{
	uint64_t v;
	__asm__ __volatile__("mov %%cr4, %0" : "=r"(v));
	return v;
}

static void write_cr4(uint64_t v)
{
	__asm__ __volatile__("mov %0, %%cr4" ::"r"(v) : "memory");
}

static void xsetbv(uint32_t index, uint64_t value)
{
	uint32_t lo = (uint32_t)value;
	uint32_t hi = (uint32_t)(value >> 32);
	__asm__ __volatile__("xsetbv" ::"c"(index), "a"(lo), "d"(hi));
}

void fpu_init_early(void)
{
	uint32_t a, b, c, d;
	struct cpu *cpu = cpu_current();

	/* CPUID leaf 1: check FXSR (EDX.24) and XSAVE (ECX.26). */
	cpuid_leaf(0x1, 0, &a, &b, &c, &d);
	bool has_fxsr = (d & (1u << 24)) != 0;
	bool has_xsave = (c & (1u << 26)) != 0;

	if (!has_fxsr) {
		panic("cpu lacks FXSR");
	}

	cpu->has_fxsr = has_fxsr;
	cpu->has_xsave = has_xsave;
	use_xsave = has_xsave;

	/*
	 * CR0: MP=1, EM=0, NE=1, TS=0.
	 * EM=0 is critical: EM=1 causes #NM on any x87/SSE instruction.
	 * TS=0 stays zero forever (eager save, no lazy-FPU).
	 */
	uint64_t cr0 = read_cr0();
	cr0 |= CR0_MP | CR0_NE;
	cr0 &= ~(CR0_EM | CR0_TS);
	write_cr0(cr0);

	/*
	 * CR4: OSFXSR=1, OSXMMEXCPT=1.
	 * OSFXSR enables FXSAVE/FXRSTOR and SSE instructions.
	 * OSXMMEXCPT enables #XF (vector 19) for unmasked SIMD exceptions.
	 */
	uint64_t cr4 = read_cr4();
	cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;

	if (has_xsave) {
		cr4 |= CR4_OSXSAVE;
	}
	write_cr4(cr4);

	if (has_xsave) {
		/* Set XCR0 = X87 | SSE = 0x3.  No AVX in v0.0.3. */
		xsetbv(0, XCR0_X87 | XCR0_SSE);

		/*
		 * Query CPUID.0DH:EBX with the current XCR0 to get the
		 * required state-area size for the enabled features.
		 */
		cpuid_leaf(0x0D, 0, &a, &b, &c, &d);
		cpu->xsave_size = b;
	} else {
		/* Legacy FXSAVE: always 512 bytes. */
		cpu->xsave_size = 512;
	}

	/*
	 * The per-task FPU buffer is fixed at 1024 bytes.
	 * With only X87+SSE enabled, CPUID.0DH should report ≤576,
	 * but defend against a bogus value to prevent buffer overflows
	 * in fpu_state_init / fpu_save / fpu_restore.
	 */
	if (cpu->xsave_size > 1024) {
		panic("fpu: xsave_size %u exceeds task buffer (1024)",
		      cpu->xsave_size);
	}
	if (cpu->xsave_size == 0) {
		panic("fpu: xsave_size is 0");
	}

	/*
	 * Prepare the canonical initial FPU state.
	 *
	 * Zero the buffer, then set:
	 *   FCW  (offset 0) = 0x037F — all x87 exceptions masked,
	 *                     precision = double-extended (same as FNINIT).
	 *   MXCSR (offset 24) = 0x1F80 — all SSE exceptions masked.
	 */
	memset(fpu_init_state_buf, 0, sizeof(fpu_init_state_buf));
	*(uint16_t *)&fpu_init_state_buf[0] = 0x037F;  /* FCW */
	*(uint32_t *)&fpu_init_state_buf[24] = 0x1F80; /* MXCSR */

	if (has_xsave) {
		/*
		 * XSAVE header at offset 512:
		 *   XSTATE_BV (bytes 512–519): mark x87 + SSE as
		 *   initialized so XRSTOR doesn't INIT them to random state.
		 */
		*(uint64_t *)&fpu_init_state_buf[512] = XCR0_X87 | XCR0_SSE;
	}

	pr_info("fpu: %s, state size=%u bytes\n",
		has_xsave ? "XSAVE" : "FXSAVE", cpu->xsave_size);
}

void fpu_state_init(void *buf)
{
	struct cpu *cpu = cpu_current();
	memcpy(buf, fpu_init_state_buf, cpu->xsave_size);
}

void fpu_save(void *buf)
{
	if (use_xsave) {
		/*
		 * XSAVE [buf], EDX:EAX = ~0 (save all enabled components).
		 * rfbm = request feature bitmap = XCR0 mask.
		 */
		__asm__ __volatile__("xsave %0"
				     : "=m"(*(uint8_t *)buf)
				     : "a"(0xFFFFFFFFu), "d"(0xFFFFFFFFu)
				     : "memory");
	} else {
		__asm__ __volatile__("fxsave %0"
				     : "=m"(*(uint8_t *)buf)
				     :
				     : "memory");
	}
}

void fpu_restore(const void *buf)
{
	if (use_xsave) {
		__asm__ __volatile__("xrstor %0"
				     :
				     : "m"(*(const uint8_t *)buf),
				       "a"(0xFFFFFFFFu), "d"(0xFFFFFFFFu)
				     : "memory");
	} else {
		__asm__ __volatile__("fxrstor %0"
				     :
				     : "m"(*(const uint8_t *)buf)
				     : "memory");
	}
}

/* ------------------------------------------------------------------ */
/* Selftest                                                           */
/* ------------------------------------------------------------------ */

/*
 * fpu_selftest: spawn two kernel-mode tasks; each loads distinct
 * XMM patterns; yield between them; assert each sees its own pattern
 * after the round-trip.
 *
 * We use movdqu to load/store a 128-bit pattern in XMM0, then compare
 * after a yield.  Since the kernel is compiled with -mgeneral-regs-only,
 * we must use inline asm for any SSE instructions.
 */

struct fpu_test_ctx {
	volatile int done;
	volatile int pass;
	uint64_t pattern_lo;
	uint64_t pattern_hi;
};

static void fpu_test_thread(void *arg)
{
	struct fpu_test_ctx *ctx = arg;
	uint64_t lo = ctx->pattern_lo;
	uint64_t hi = ctx->pattern_hi;
	_Alignas(16) uint8_t load_buf[16];
	_Alignas(16) uint8_t store_buf[16];
	*(uint64_t *)&load_buf[0] = lo;
	*(uint64_t *)&load_buf[8] = hi;

	/* Load pattern into XMM0. */
	__asm__ __volatile__("movdqu %0, %%xmm0" : : "m"(load_buf) : "memory");

	for (int i = 0; i < 8; i++) {
		sched_yield();
	}

	__asm__ __volatile__("movdqu %%xmm0, %0"
			     : "=m"(store_buf)
			     :
			     : "memory");

	uint64_t got_lo = *(uint64_t *)&store_buf[0];
	uint64_t got_hi = *(uint64_t *)&store_buf[8];

	if (got_lo == lo && got_hi == hi) {
		ctx->pass = 1;
	} else {
		pr_err("fpu: selftest FAIL: expected 0x%016lx_%016lx, "
		       "got 0x%016lx_%016lx\n",
		       (unsigned long)hi, (unsigned long)lo,
		       (unsigned long)got_hi, (unsigned long)got_lo);
		ctx->pass = 0;
	}
	ctx->done = 1;
}

int fpu_selftest(void)
{
	struct fpu_test_ctx ctx_a = {
	    .done = 0,
	    .pass = 0,
	    .pattern_lo = 0xDEADBEEFCAFEBABEull,
	    .pattern_hi = 0x1111111122222222ull,
	};
	struct fpu_test_ctx ctx_b = {
	    .done = 0,
	    .pass = 0,
	    .pattern_lo = 0xAAAAAAAABBBBBBBBull,
	    .pattern_hi = 0xCCCCCCCCDDDDDDDDull,
	};

	int err;

	err = sched_create_kernel_thread("fpu-test-a", fpu_test_thread,
					 (void *)&ctx_a, NULL);
	if (err) {
		return err;
	}

	err = sched_create_kernel_thread("fpu-test-b", fpu_test_thread,
					 (void *)&ctx_b, NULL);
	if (err) {
		return err;
	}

	while (!ctx_a.done || !ctx_b.done) {
		sched_yield();
	}

	if (!ctx_a.pass || !ctx_b.pass) {
		return -1;
	}

	return 0;
}
