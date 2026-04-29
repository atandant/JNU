/*
 * kernel/arch/x86_64/cpu.c — CPUID feature detection and per-CPU bring-up.
 *
 * Verifies the v0.0.1 baseline (long mode, MSR, APIC, NX, TSC), sets the
 * required control-register bits (CR0.WP, CR4.PGE, EFER.NXE, optionally
 * CR4.SMEP/SMAP), and calibrates the TSC against the PIT for klog
 * timestamps.
 *
 * Reference: Intel SDM Vol. 2A (CPUID), Vol. 3 §2.5 (CRn), §17.17 (TSC).
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/cpu.h>
#include <jnu/io.h>
#include <jnu/klog.h>
#include <jnu/panic.h>
#include <jnu/string.h>
#include <jnu/types.h>

static struct cpu boot_cpu;

struct cpu *cpu_current(void)
{
	return &boot_cpu;
}

static void cpuid(uint32_t leaf,
		  uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
	__asm__ __volatile__ ("cpuid"
			      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
			      : "a"(leaf), "c"(0));
}

static uint64_t read_cr0(void) { uint64_t v; __asm__ __volatile__ ("mov %%cr0, %0" : "=r"(v)); return v; }
static uint64_t read_cr4(void) { uint64_t v; __asm__ __volatile__ ("mov %%cr4, %0" : "=r"(v)); return v; }
static void write_cr0(uint64_t v) { __asm__ __volatile__ ("mov %0, %%cr0" :: "r"(v) : "memory"); }
static void write_cr4(uint64_t v) { __asm__ __volatile__ ("mov %0, %%cr4" :: "r"(v) : "memory"); }

void cpu_init(void)
{
	uint32_t a, b, c, d;

	cpuid(0x1, &a, &b, &c, &d);
	bool has_apic = (d & (1u << 9)) != 0;
	bool has_msr  = (d & (1u << 5)) != 0;
	bool has_tsc  = (d & (1u << 4)) != 0;

	cpuid(0x80000001, &a, &b, &c, &d);
	bool has_nx   = (d & (1u << 20)) != 0;
	bool has_lm   = (d & (1u << 29)) != 0;

	cpuid(0x7, &a, &b, &c, &d);
	bool has_smep = (b & (1u << 7))  != 0;
	bool has_smap = (b & (1u << 20)) != 0;

	if (!has_lm)   { panic("cpu: long mode required"); }
	if (!has_msr)  { panic("cpu: MSR support required"); }
	if (!has_apic) { panic("cpu: APIC required"); }
	if (!has_tsc)  { panic("cpu: TSC required"); }
	if (!has_nx)   { panic("cpu: NX required"); }

	boot_cpu.has_apic = has_apic;
	boot_cpu.has_nx   = has_nx;
	boot_cpu.has_smep = has_smep;
	boot_cpu.has_smap = has_smap;

	/* CR0.WP — kernel writes to ring-0 RO pages fault. */
	write_cr0(read_cr0() | CR0_WP);

	/* CR4.PGE — global pages survive CR3 reloads. */
	uint64_t cr4 = read_cr4() | CR4_PGE;
	if (has_smep) { cr4 |= CR4_SMEP; }
	if (has_smap) { cr4 |= CR4_SMAP; }
	write_cr4(cr4);

	/* EFER.NXE — honor PTE bit 63. */
	wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);

	/* Wire IA32_GS_BASE to the per-CPU block. */
	wrmsr(MSR_GS_BASE, (uint64_t)(uintptr_t)&boot_cpu);

	pr_info("cpu: features lm=%c apic=%c nx=%c smep=%c smap=%c\n",
		has_lm   ? 'y' : 'n',
		has_apic ? 'y' : 'n',
		has_nx   ? 'y' : 'n',
		has_smep ? 'y' : 'n',
		has_smap ? 'y' : 'n');
}

/* ------------------------------------------------------------------------- */
/* TSC calibration via PIT one-shot                                          */
/* ------------------------------------------------------------------------- */

/*
 * The PIT runs at 1.193182 MHz. We program channel 2 (the speaker
 * channel — its gate is software-controllable via port 0x61, which
 * makes it the canonical calibration channel even on machines that
 * route channel 0 to the IOAPIC) in mode 0 (one-shot) for a known
 * count, busy-wait until the counter latches its done state, and
 * sample TSC before and after.
 *
 * Reference: Intel 8254 datasheet, Linux arch/x86/kernel/tsc.c
 * `pit_calibrate_tsc` for the same approach.
 */

#define PIT_HZ		1193182u
#define PIT_CMD		0x43
#define PIT_CH2		0x42
#define PORT_NMI_SC	0x61



void cpu_calibrate_tsc(void)
{
	const uint32_t us = 10000;	/* 10 ms */
	const uint32_t count = (PIT_HZ * us + 500000u) / 1000000u;

	if (count == 0 || count > 0xFFFFu) {
		panic("cpu: invalid PIT calibration count");
	}

	/* Disable speaker output, enable channel 2 gate. */
	uint8_t sc = inb(PORT_NMI_SC);
	sc = (uint8_t)((sc & ~0x02u) | 0x01u);
	outb(PORT_NMI_SC, sc);

	/* Channel 2, lobyte/hibyte, mode 0 (one-shot), binary. */
	outb(PIT_CMD, 0xB0);
	outb(PIT_CH2, (uint8_t)(count & 0xFFu));
	outb(PIT_CH2, (uint8_t)((count >> 8) & 0xFFu));

	uint64_t t0 = cpu_rdtsc();
	while ((inb(PORT_NMI_SC) & 0x20) == 0) {
		__asm__ __volatile__ ("pause");
	}
	uint64_t t1 = cpu_rdtsc();

	uint64_t per_us = (t1 - t0) / us;
	if (per_us == 0) {
		per_us = 1;
	}
	boot_cpu.tsc_per_us = per_us;

	pr_info("cpu: tsc ~%lu MHz (%lu ticks/us)\n",
		(unsigned long)per_us,
		(unsigned long)per_us);
}

uint64_t cpu_us_since_boot(void)
{
	if (boot_cpu.tsc_per_us == 0) {
		return 0;
	}
	return cpu_rdtsc() / boot_cpu.tsc_per_us;
}
