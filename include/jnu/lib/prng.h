/*
 * include/jnu/lib/prng.h — Kernel pseudo-random number generator.
 *
 * Provides a fast, non-cryptographic PRNG for kernel use cases that
 * need randomness but not cryptographic strength: ASLR offsets, hash
 * table salts, jitter-based tie-breaking.
 *
 * The PRNG is seeded once at boot from hardware entropy (RDRAND with
 * RDTSC+HPET fallback, mixed with a multiplicative hash).  After
 * seeding, prng_u64() returns xorshift64* values.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/base/types.h>

/*
 * Seed the global PRNG from hardware entropy.  Must be called after
 * cpu_init() and hpet_init() (needs RDTSC and optionally HPET).
 * Called once from kernel main().
 */
void prng_seed(void);

/* Hardware entropy source for the PRNG. */
uint64_t entropy_get_seed(void);

/*
 * Return a uniformly distributed 64-bit pseudo-random value.
 * Not cryptographically secure.  Not reentrant — call sites that
 * race on SMP will need per-CPU state (future work).
 */
uint64_t prng_u64(void);

/*
 * Return a uniformly distributed value in [0, bound).
 * `bound` must be > 0.
 */
uint64_t prng_bounded(uint64_t bound);

/*
 * Return a page-aligned random offset in [0, max_pages * PAGE_SIZE).
 * Used by ASLR to randomize stack and mmap bases.
 */
uint64_t prng_page_offset(uint64_t max_pages);
