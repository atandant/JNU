/*
 * kernel/arch/x86_64/entropy.c — Hardware entropy collection.
 *
 * Gathers raw entropy from the best available x86-64 source, in order:
 *
 *   1. RDSEED  (CPUID.07H:EBX bit 18) — true-entropy source.
 *   2. RDRAND  (CPUID.01H:ECX bit 30) — reseeded hardware CSPRNG.
 *   3. Fallback: RDTSC jitter ^ HPET counter, mixed with a Knuth
 *      multiplicative hash to decorrelate timing patterns.
 *
 * The output is raw and biased: entropy_collect() exposes it to the
 * kernel CSPRNG (lib/csprng.c), which conditions it through BLAKE2s
 * before use as key material.  This code is architecture-specific and
 * lives under arch/x86_64/.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/arch/cpu.h>
#include <jnu/base/types.h>
#include <jnu/drivers/hpet.h>
#include <jnu/lib/csprng.h>
#include <jnu/lib/string.h>

/* CPUID.01H:ECX bit 30 — RDRAND instruction supported. */
#define CPUID_01_ECX_RDRAND (1u << 30)
/* CPUID.07H:EBX bit 18 — RDSEED instruction supported. */
#define CPUID_07_EBX_RDSEED (1u << 18)

static bool has_rdrand;
static bool has_rdseed;
static bool features_detected;

/*
 * Detect RDRAND and RDSEED support.  Idempotent; the first call probes
 * CPUID and caches the result.
 */
static void detect_features(void)
{
	uint32_t a, b, c, d;

	if (features_detected) {
		return;
	}

	__asm__ __volatile__("cpuid"
			     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
			     : "a"(1), "c"(0));
	has_rdrand = (c & CPUID_01_ECX_RDRAND) != 0;

	__asm__ __volatile__("cpuid"
			     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
			     : "a"(7), "c"(0));
	has_rdseed = (b & CPUID_07_EBX_RDSEED) != 0;

	features_detected = true;
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
 * Try to read a 64-bit value via RDSEED — the true-entropy source
 * (RDRAND is a reseeded CSPRNG, RDSEED draws directly from the entropy
 * conditioner).  RDSEED depletes faster, so retry more generously.
 * Returns true on success and writes the value to *out.
 */
static bool try_rdseed64(uint64_t *out)
{
	for (int i = 0; i < 32; i++) {
		uint64_t val;
		uint8_t ok;

		__asm__ __volatile__("rdseed %0; setc %1"
				     : "=r"(val), "=qm"(ok));
		if (ok) {
			*out = val;
			return true;
		}
		__asm__ __volatile__("pause");
	}
	return false;
}

/*
 * Knuth multiplicative hash — decorrelates correlated timing values.
 * The constant is 2^64 / phi (golden ratio).
 */
static uint64_t knuth_mix(uint64_t v) { return v * 11400714819323198485ull; }

/*
 * Produce one 64-bit raw entropy word from the best available source,
 * always folding in TSC timing.  The output is NOT uniform — callers
 * must condition it (e.g. through a hash) before use as key material.
 */
static uint64_t entropy_word(void)
{
	uint64_t v;

	detect_features();

	if (has_rdseed && try_rdseed64(&v)) {
		return v ^ knuth_mix(cpu_rdtsc());
	}
	if (has_rdrand && try_rdrand64(&v)) {
		return v ^ knuth_mix(cpu_rdtsc());
	}

	/* Fallback: accumulate TSC jitter, fold in HPET if present. */
	v = 0;
	for (int i = 0; i < 64; i++) {
		__asm__ __volatile__("pause");
		v ^= knuth_mix(cpu_rdtsc());
	}
	if (hpet_available()) {
		v ^= knuth_mix(hpet_read_counter());
	}
	return v;
}

/*
 * Fill `buf` with `len` bytes of raw hardware entropy, one 64-bit word
 * at a time.  See include/jnu/lib/csprng.h — the bytes are biased and
 * MUST be conditioned (BLAKE2s) before becoming key material.
 */
void entropy_collect(void *buf, size_t len)
{
	uint8_t *p = (uint8_t *)buf;

	while (len) {
		uint64_t w = entropy_word();
		size_t n = len < sizeof(w) ? len : sizeof(w);

		memcpy(p, &w, n);
		p += n;
		len -= n;
	}
}
