/*
 * kernel/syscall/sys_clock_gettime.c — clock_gettime syscall.
 *
 * v0.0.3 §2.9:
 *   CLOCK_REALTIME  = boot RTC + TSC delta.
 *   CLOCK_MONOTONIC = TSC delta from boot.
 *   Other clock IDs return -EINVAL.
 *
 * Correctness depends on TSC calibration (already present via PIT in
 * v0.0.2) and the boot-time RTC snapshot from rtc_init().
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/arch/cpu.h>
#include <jnu/base/types.h>
#include <jnu/drivers/rtc.h>
#include <jnu/user/syscall.h>
#include <jnu/user/usercopy.h>
#include <uapi/jnu/errno.h>

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1

struct timespec {
	int64_t tv_sec;
	int64_t tv_nsec;
};

/*
 * Convert a struct tm to seconds since the Unix epoch (1970-01-01).
 * Simplified: no leap-second handling, assumes UTC, valid for dates
 * after 2000-01-01 which is the only range the CMOS RTC reports.
 */
static int64_t tm_to_epoch(const struct tm *t)
{
	static const uint16_t mdays[] = {0,   31,  59,	90,  120, 151,
					 181, 212, 243, 273, 304, 334};
	int64_t y = t->year;
	int64_t m = t->month;
	int64_t d = t->day;
	int64_t days;

	/* Adjust for months January/February for leap-year calculation. */
	if (m <= 2) {
		y--;
	}

	days = 365 * (y - 1970) + (y - 1969) / 4 - (y - 1901) / 100 +
	       (y - 1601) / 400;
	if (m >= 1 && m <= 12) {
		days += mdays[m - 1];
	}
	days += d - 1;

	return days * 86400ll + t->hour * 3600ll + t->minute * 60ll + t->second;
}

int64_t sys_clock_gettime(int clockid, void *utp)
{
	struct timespec ts;
	uint64_t us;
	int err;

	if (!utp) {
		return -EINVAL;
	}

	us = cpu_us_since_boot();

	switch (clockid) {
	case CLOCK_MONOTONIC:
		ts.tv_sec = (int64_t)(us / 1000000ull);
		ts.tv_nsec = (int64_t)((us % 1000000ull) * 1000ull);
		break;

	case CLOCK_REALTIME: {
		struct tm boot;
		int64_t boot_epoch;

		rtc_now(&boot);
		boot_epoch = tm_to_epoch(&boot);
		us += (uint64_t)boot_epoch * 1000000ull;
		ts.tv_sec = (int64_t)(us / 1000000ull);
		ts.tv_nsec = (int64_t)((us % 1000000ull) * 1000ull);
		break;
	}

	default:
		return -EINVAL;
	}

	err = copy_to_user(utp, &ts, sizeof(ts));
	if (err) {
		return (int64_t)err;
	}
	return 0;
}
