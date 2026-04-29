/*
 * include/jnu/lapic_timer.h — Local APIC timer as the v0.0.2 scheduler
 * tick.
 *
 * Per jnuspec2.md §2.7, the LAPIC timer replaces the PIT as the
 * preemption tick once it is up. This driver runs in one of two modes
 * depending on the CPU:
 *
 *   - TSC-deadline mode (CPUID.01h:ECX[24] = 1, e.g. QEMU q35 host):
 *     the LVT timer entry has its mode set to TSC-deadline and an IRQ
 *     fires when the TSC reaches the value written to
 *     IA32_TSC_DEADLINE. The handler re-arms with the next deadline.
 *
 *   - Periodic mode (everything else):
 *     LAPIC bus frequency is calibrated against the already-known TSC
 *     once at boot, the divide configuration is set to /1, and the
 *     initial-count register is loaded with bus_hz / TICK_HZ.
 *
 * Either way the IRQ lands on VEC_LAPIC_TIMER and calls sched_tick().
 * After lapic_timer_init() returns, the spec requires that PIT channel 0
 * stop being a tick source — main.c masks the PIT IOAPIC pin then.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>

/*
 * Scheduler tick rate. Spec §2.7 mandates a 10 ms quantum; we run the
 * tick at 100 Hz and the scheduler counts ticks toward its quantum.
 */
#define LAPIC_TIMER_HZ		100

/*
 * Initialize the LAPIC timer at LAPIC_TIMER_HZ. Must run after
 * apic_init() (so we have an LAPIC MMIO base) and after
 * cpu_calibrate_tsc() (so TSC frequency is known and is available as
 * the calibration reference). Installs the IRQ handler on
 * VEC_LAPIC_TIMER. Does not enable interrupts; the caller is expected
 * to leave that to the scheduler / idle loop.
 */
void lapic_timer_init(void);

/* True if TSC-deadline mode is in use. */
bool lapic_timer_is_deadline_mode(void);

/* Number of timer IRQs delivered since lapic_timer_init(). */
uint64_t lapic_timer_ticks(void);

/* Selftest: enable interrupts briefly and confirm tick count advances. */
int lapic_timer_selftest(void);
