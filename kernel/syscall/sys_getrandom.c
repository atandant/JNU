/*
 * kernel/syscall/sys_getrandom.c — getrandom syscall.
 *
 * v0.0.3 §2.9: TSC-mixed xorshift64 PRNG seeded at boot from TSC + RTC.
 * **Documented as not cryptographic.**  musl uses getrandom during
 * stack-protector initialization to obtain a canary value; deterministic
 * but distinct-per-boot bytes are better than refusing with -ENOSYS.
 * Real CSPRNG lands in v0.0.5+.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/cpu.h>
#include <jnu/errno.h>
#include <jnu/rtc.h>
#include <jnu/syscall.h>
#include <jnu/types.h>
#include <jnu/usercopy.h>

static uint64_t prng_state;
static bool prng_seeded;

/*
 * Seed once from TSC + RTC. The result is deterministic per-boot but
 * varies between boots (different boot times, different TSC values).
 */
static void seed_once(void)
{
	struct tm t;

	if (prng_seeded) {
		return;
	}

	rtc_now(&t);
	prng_state = cpu_rdtsc();
	prng_state ^= ((uint64_t)t.year << 40) | ((uint64_t)t.month << 32) |
		      ((uint64_t)t.day << 24) | ((uint64_t)t.hour << 16) |
		      ((uint64_t)t.minute << 8) | t.second;

	/* Ensure non-zero seed for xorshift64. */
	if (prng_state == 0) {
		prng_state = 0xDEADBEEFCAFEBABEull;
	}
	prng_seeded = true;
}

static uint64_t xorshift64(void)
{
	uint64_t x = prng_state;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	prng_state = x;
	return x;
}

int64_t sys_getrandom(void *ubuf, size_t buflen, unsigned int flags)
{
	uint8_t chunk[64];
	size_t done = 0;

	(void)flags;

	if (!ubuf || buflen == 0) {
		return -EINVAL;
	}

	seed_once();

	while (done < buflen) {
		size_t want = buflen - done;
		size_t i;
		int err;

		if (want > sizeof(chunk)) {
			want = sizeof(chunk);
		}

		for (i = 0; i + 8 <= want; i += 8) {
			uint64_t val = xorshift64();
			chunk[i] = (uint8_t)(val);
			chunk[i + 1] = (uint8_t)(val >> 8);
			chunk[i + 2] = (uint8_t)(val >> 16);
			chunk[i + 3] = (uint8_t)(val >> 24);
			chunk[i + 4] = (uint8_t)(val >> 32);
			chunk[i + 5] = (uint8_t)(val >> 40);
			chunk[i + 6] = (uint8_t)(val >> 48);
			chunk[i + 7] = (uint8_t)(val >> 56);
		}

		if (i < want) {
			uint64_t val = xorshift64();
			while (i < want) {
				chunk[i++] = (uint8_t)(val & 0xFF);
				val >>= 8;
			}
		}

		err = copy_to_user((uint8_t *)ubuf + done, chunk, want);
		if (err) {
			return done > 0 ? (int64_t)done : (int64_t)err;
		}
		done += want;
	}

	return (int64_t)done;
}
