/*
 * include/jnu/rtc.h — CMOS Real-Time Clock driver.
 *
 * Reads the MC146818-compatible CMOS RTC once at boot to obtain the
 * wall-clock date and time. Handles both BCD and binary modes via
 * Status Register B bit 2.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>

struct tm {
	uint16_t	year;		/* e.g. 2026 */
	uint8_t		month;		/* 1–12 */
	uint8_t		day;		/* 1–31 */
	uint8_t		hour;		/* 0–23 */
	uint8_t		minute;		/* 0–59 */
	uint8_t		second;		/* 0–59 */
};

/*
 * Read the CMOS RTC and print the boot wall time at INFO level.
 * Call once during early boot; the clock is not polled again.
 */
void rtc_init(void);

/* Snapshot the last-read time into `out`. */
void rtc_now(struct tm *out);
