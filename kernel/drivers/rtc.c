/*
 * kernel/drivers/rtc.c — CMOS Real-Time Clock (MC146818), boot-time read.
 *
 * Reads the CMOS RTC once at boot to obtain the wall-clock date and
 * time. Handles both BCD and binary modes via Status Register B bit 2,
 * and 12-hour vs 24-hour via bit 1. The result is logged at INFO and
 * available via rtc_now().
 *
 * No IRQ is used; we poll the Update-In-Progress flag (Register A
 * bit 7) to ensure a consistent snapshot. This driver is read-only
 * and never writes back to the CMOS.
 *
 * Reference: MC146818 datasheet, OSDev wiki "CMOS".
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/base/types.h>
#include <jnu/drivers/io.h>
#include <jnu/drivers/rtc.h>
#include <jnu/lib/klog.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

/* CMOS register indices. */
#define RTC_SECONDS 0x00
#define RTC_MINUTES 0x02
#define RTC_HOURS 0x04
#define RTC_DAY 0x07
#define RTC_MONTH 0x08
#define RTC_YEAR 0x09
#define RTC_CENTURY 0x32 /* may not exist on all boards */
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B

static struct tm boot_time;

static uint8_t cmos_read(uint8_t reg)
{
	/*
	 * Bit 7 of the address port controls NMI masking. We preserve
	 * NMI enabled (bit 7 = 0) to avoid accidentally masking NMIs.
	 */
	outb(CMOS_ADDR, (uint8_t)(reg & 0x7Fu));
	return inb(CMOS_DATA);
}

static uint8_t bcd_to_bin(uint8_t bcd)
{
	return (uint8_t)((bcd / 16) * 10 + (bcd % 16));
}

/*
 * Wait until the RTC update-in-progress bit clears to avoid reading
 * partial values while the RTC is latching a new second.
 */
static void wait_for_update(void)
{
	while (cmos_read(RTC_STATUS_A) & 0x80u) {
		__asm__ __volatile__("pause");
	}
}

void rtc_init(void)
{
	wait_for_update();

	uint8_t sec = cmos_read(RTC_SECONDS);
	uint8_t min = cmos_read(RTC_MINUTES);
	uint8_t hour = cmos_read(RTC_HOURS);
	uint8_t day = cmos_read(RTC_DAY);
	uint8_t month = cmos_read(RTC_MONTH);
	uint8_t year = cmos_read(RTC_YEAR);

	uint8_t status_b = cmos_read(RTC_STATUS_B);
	bool is_bcd = (status_b & 0x04u) == 0;
	bool is_12h = (status_b & 0x02u) == 0;

	if (is_bcd) {
		sec = bcd_to_bin(sec);
		min = bcd_to_bin(min);
		hour = bcd_to_bin((uint8_t)(hour & 0x7Fu));
		day = bcd_to_bin(day);
		month = bcd_to_bin(month);
		year = bcd_to_bin(year);
	} else {
		hour = (uint8_t)(hour & 0x7Fu);
	}

	/* Handle 12-hour mode: bit 7 of the raw hour byte means PM. */
	if (is_12h && (cmos_read(RTC_HOURS) & 0x80u)) {
		hour = (uint8_t)((hour % 12) + 12);
	}

	/*
	 * CMOS year is 00–99. Assume century 20xx. The century register
	 * (0x32) is unreliable across hardware, so we hardcode 2000.
	 */
	boot_time.year = (uint16_t)(2000u + year);
	boot_time.month = month;
	boot_time.day = day;
	boot_time.hour = hour;
	boot_time.minute = min;
	boot_time.second = sec;

	pr_info("rtc: %04u-%02u-%02u %02u:%02u:%02u UTC\n",
		(unsigned)boot_time.year, (unsigned)boot_time.month,
		(unsigned)boot_time.day, (unsigned)boot_time.hour,
		(unsigned)boot_time.minute, (unsigned)boot_time.second);
}

void rtc_now(struct tm *out) { *out = boot_time; }

static bool rtc_is_leap(uint16_t year)
{
	return (year % 4u) == 0 && ((year % 100u) != 0 || (year % 400u) == 0);
}

uint32_t rtc_now_unix(void)
{
	static const uint16_t month_days[] = {
	    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
	};
	uint32_t days = 0;

	for (uint16_t year = 1970; year < boot_time.year; year++)
		days += rtc_is_leap(year) ? 366u : 365u;
	for (uint8_t month = 1; month < boot_time.month; month++) {
		days += month_days[month - 1u];
		if (month == 2 && rtc_is_leap(boot_time.year))
			days++;
	}
	days += (uint32_t)boot_time.day - 1u;

	return days * 86400u + (uint32_t)boot_time.hour * 3600u +
	       (uint32_t)boot_time.minute * 60u + (uint32_t)boot_time.second;
}
