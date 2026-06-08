/*
 * kernel/arch/x86_64/entropy.c — Hardware entropy collection.
 *
 * Gathers raw entropy from the best available x86-64 source:
 *
 *   1. RDRAND  (CPUID.01H:ECX bit 30) — true hardware RNG.
 *   2. Fallback: RDTSC ^ HPET counter, mixed with a Knuth
 *      multiplicative hash to decorrelate timing patterns.
 *
 * The result is used to seed the kernel PRNG (lib/prng.c).  This
 * code is architecture-specific and lives under arch/x86_64/.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/arch/cpu.h>
#include <jnu/base/types.h>
#include <jnu/drivers/hpet.h>
#include <jnu/lib/klog.h>
#include <jnu/lib/prng.h>

/* CPUID.01H:ECX bit 30 — RDRAND instruction supported. */
#define CPUID_01_ECX_RDRAND (1u << 30)

static bool has_rdrand;

/*
 * Detect RDRAND support.  Called once from entropy_get_seed().
 */
static void detect_rdrand(void)
{
	uint32_t a, b, c, d;

	__asm__ __volatile__("cpuid"
			     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
			     : "a"(1), "c"(0));
	has_rdrand = (c & CPUID_01_ECX_RDRAND) != 0;
}

/*
 * Try to read a 64-bit random value via RDRAND.  Retries up to 10
 * times (Intel recommends at least 10 retries before giving up).
 * Returns true on success and writes the value to *out.
 */
static bool try_rdrand64(uint64_t *out)
{
	for (int i = 0; i < 10; i++) {
		uint64_t val;
		uint8_t ok;

		__asm__ __volatile__("rdrand %0; setc %1"
				     : "=r"(val), "=qm"(ok));
		if (ok) {
			*out = val;
			return true;
		}
	}
	return false;
}

/*
 * Knuth multiplicative hash — decorrelates correlated timing values.
 * The constant is 2^64 / phi (golden ratio).
 */
static uint64_t knuth_mix(uint64_t v) { return v * 11400714819323198485ull; }

/*
 * Collect a 64-bit entropy seed from the best available source.
 *
 * Priority:
 *   1. RDRAND — true hardware entropy.
 *   2. RDTSC ^ HPET counter — timing jitter, mixed via Knuth hash.
 *   3. RDTSC alone (HPET unavailable) — weakest, but still varies
 *      per boot on real hardware.
 */
uint64_t entropy_get_seed(void)
{
	uint64_t seed = 0;

	detect_rdrand();

	if (has_rdrand) {
		uint64_t r1;

		if (try_rdrand64(&r1)) {
			seed = r1 ^ knuth_mix(cpu_rdtsc());
			pr_info("entropy: seeded from RDRAND + RDTSC\n");
			goto finalize;
		}
		/* RDRAND failed despite being advertised — fall through. */
	}

	/* Fallback: accumulate RDTSC jitter. */
	uint64_t acc = 0;
	for (int i = 0; i < 64; i++) {
		__asm__ __volatile__("pause");
		acc ^= knuth_mix(cpu_rdtsc());
	}
	seed = acc;

	if (hpet_available()) {
		seed ^= knuth_mix(hpet_read_counter());
		pr_info("entropy: seeded from RDTSC jitter + HPET\n");
	} else {
		pr_warn("entropy: seeded from RDTSC jitter only "
			"(weak — ASLR may be predictable)\n");
	}

finalize:

	seed ^= seed >> 33;
	seed *= 0xff51afd7ed558ccdull;
	seed ^= seed >> 33;
	seed *= 0xc4ceb9fe1a85ec53ull;
	seed ^= seed >> 33;

	return seed;
}
