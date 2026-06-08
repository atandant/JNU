/*
 * kernel/lib/prng.c — xorshift64* pseudo-random number generator.
 *
 * Platform-agnostic PRNG seeded from arch-specific entropy.  The
 * xorshift64* variant has better statistical properties than plain
 * xorshift64 (passes BigCrush) and is fast enough for kernel hot
 * paths like ASLR offset generation.
 *
 * Reference: Marsaglia, "Xorshift RNGs" (2003); Vigna, "An
 * experimental exploration of Marsaglia's xorshift generators" (2016).
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/base/types.h>
#include <jnu/lib/klog.h>
#include <jnu/lib/prng.h>
#include <jnu/mm/paging.h>

/*
 * Global PRNG state.  Not reentrant — fine for single-CPU.
 * SMP would need per-CPU state or a spinlock.
 */
static uint64_t prng_state;

void prng_seed(void)
{
	prng_state = entropy_get_seed();

	/*
	 * xorshift64* requires state != 0.  If the entropy source
	 * somehow returned zero, use a fixed non-zero fallback.
	 */
	if (prng_state == 0) {
		prng_state = 0x853c49e6748fea9bull;
	}

	/*
	 * Discard the first few outputs to let the state diffuse.
	 * This mitigates weak seeds where only a few bits carry
	 * real entropy.
	 */
	for (int i = 0; i < 16; i++) {
		(void)prng_u64();
	}

	pr_info("prng: xorshift64* seeded\n");
}

uint64_t prng_u64(void)
{
	uint64_t x = prng_state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	prng_state = x;

	return x * 0x2545f4914f6cdd1dull;
}

uint64_t prng_bounded(uint64_t bound)
{
	uint64_t threshold;
	uint64_t r;

	if (bound <= 1) {
		return 0;
	}

	/*
	 * Rejection sampling to avoid modulo bias.
	 * threshold = (2^64 - bound) % bound, but computed without
	 * 128-bit arithmetic.
	 */
	threshold = (-bound) % bound;

	do {
		r = prng_u64();
	} while (r < threshold);

	return r % bound;
}

uint64_t prng_page_offset(uint64_t max_pages)
{
	if (max_pages == 0) {
		return 0;
	}
	return prng_bounded(max_pages) * PAGE_SIZE;
}
