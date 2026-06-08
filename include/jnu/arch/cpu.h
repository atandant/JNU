/*
 * include/jnu/arch/cpu.h — CPU feature detection and per-CPU block.
 *
 * Phase 2 brings up CPU-side state: CPUID feature bits, control registers
 * (CR0/CR4/EFER), TSC calibration, and the per-CPU `struct cpu` reachable
 * via `IA32_GS_BASE`. v0.0.1 has exactly one CPU.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/base/types.h>

struct cpu {
	uint32_t id;	     /* APIC id of this CPU */
	uint64_t tsc_per_us; /* TSC ticks per microsecond */
	bool has_smep;
	bool has_smap;
	bool has_nx;
	bool has_apic;
	bool has_fxsr;	     /* CPUID.01H:EDX.FXSR (bit 24) */
	bool has_xsave;	     /* CPUID.01H:ECX.XSAVE (bit 26) */
	uint32_t xsave_size; /* bytes needed for XSAVE/FXSAVE area */
};

/*
 * One-shot CPU bring-up. Detects required features, sets EFER.NXE,
 * CR0.WP, CR4.PGE, and (if available) CR4.SMEP, CR4.SMAP. Calibrates
 * the TSC against the PIT (cpu_calibrate_tsc must be called after
 * pit_init). Wires IA32_GS_BASE to point at the per-CPU block.
 *
 * Panics if any required feature (long mode, APIC, NX, MSR, TSC) is
 * absent.
 */
void cpu_init(void);

/* Run after pit_init(): calibrates tsc_per_us. */
void cpu_calibrate_tsc(void);

struct cpu *cpu_current(void);

/* Read the timestamp counter. */
static inline uint64_t cpu_rdtsc(void)
{
	uint32_t lo, hi;
	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}

/* Microseconds since boot, derived from TSC and tsc_per_us. */
uint64_t cpu_us_since_boot(void);

/* MSR helpers. */
static inline uint64_t rdmsr(uint32_t msr)
{
	uint32_t lo, hi;
	__asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v)
{
	uint32_t lo = (uint32_t)v;
	uint32_t hi = (uint32_t)(v >> 32);
	__asm__ __volatile__("wrmsr" ::"a"(lo), "d"(hi), "c"(msr));
}

#define MSR_EFER 0xC0000080
#define MSR_FS_BASE 0xC0000100
#define MSR_GS_BASE 0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102
#define MSR_APIC_BASE 0x0000001B

#define EFER_NXE (1ull << 11)

#define CR0_MP (1ull << 1)
#define CR0_EM (1ull << 2)
#define CR0_TS (1ull << 3)
#define CR0_NE (1ull << 5)
#define CR0_WP (1ull << 16)

#define CR4_OSFXSR (1ull << 9)
#define CR4_OSXMMEXCPT (1ull << 10)
#define CR4_PGE (1ull << 7)
#define CR4_SMEP (1ull << 20)
#define CR4_SMAP (1ull << 21)
#define CR4_OSXSAVE (1ull << 18)

/* XCR0 feature bits (Extended Control Register 0). */
#define XCR0_X87 (1ull << 0)
#define XCR0_SSE (1ull << 1)

/*
 * FPU state management — v0.0.3 §2.7.
 *
 * fpu_init_early() detects FXSR/XSAVE, sets the CR0/CR4 bits, and
 * prepares the canonical initial FPU state buffer.  Called once from
 * cpu_init().
 *
 * fpu_state_init() copies the canonical starting state into a task's
 * FPU buffer.  Called at task creation.
 *
 * fpu_save(buf) / fpu_restore(buf) perform XSAVE/XRSTOR or
 * FXSAVE/FXRSTOR into/from the given buffer.  Called by the
 * context-switch path in sched.c.
 */
void fpu_init_early(void);
void fpu_state_init(void *buf);
void fpu_save(void *buf);
void fpu_restore(const void *buf);

/* Phase 1 selftest: two tasks load distinct XMM patterns, yield, check. */
int fpu_selftest(void);
