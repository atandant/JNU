/*
 * include/jnu/drivers/pit.h — 8254 PIT timer driver.
 *
 * Channel 0 runs at 100 Hz in rate-generator mode (mode 2), routed
 * through the IOAPIC to vector 32. A 64-bit jiffies counter gives
 * a coarse monotonic clock until the LAPIC timer replaces it in v0.0.2.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/base/types.h>

#define PIT_FREQUENCY_HZ 100

/*
 * Initialize PIT channel 0 at 100 Hz and install the timer IRQ handler
 * on vector 32 via ioapic_route_isa_irq.
 */
void pit_init(void);

/* Current monotonic tick count (incremented 100 times/s). */
uint64_t pit_get_ticks(void);

/* Busy-wait for approximately `ms` milliseconds. */
void pit_sleep_ms(uint32_t ms);
