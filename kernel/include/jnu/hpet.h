/*
 * include/jnu/hpet.h — HPET (High Precision Event Timer) driver.
 *
 * Used for two purposes:
 *   1. High-precision TSC calibration (replaces PIT channel 2 polling).
 *   2. Monotonic nanosecond-resolution counter via hpet_read_ns().
 *
 * Not used as a tick source — the LAPIC timer owns that role.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>

/*
 * Discover and map the HPET from the ACPI "HPET" table.
 * Must be called after paging_init() and apic_init() (needs HHDM).
 * Returns 0 on success, negative errno if no HPET is present.
 */
int hpet_init(uint64_t rsdp_phys, uint64_t hhdm_offset);

/* True once hpet_init() has succeeded. */
bool hpet_available(void);

/* Read the HPET main counter (raw ticks). */
uint64_t hpet_read_counter(void);

/* Convert HPET ticks to nanoseconds. */
uint64_t hpet_ticks_to_ns(uint64_t ticks);

/* Busy-wait for `us` microseconds using the HPET counter. */
void hpet_udelay(uint64_t us);

/* Nanoseconds since HPET was enabled. */
uint64_t hpet_read_ns(void);

/* Microseconds since HPET was enabled (convenience). */
uint64_t hpet_read_us(void);
