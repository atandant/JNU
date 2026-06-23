/*
 * include/jnu/lib/csprng.h — Cryptographically secure PRNG.
 *
 * A real CSPRNG for the security-sensitive randomness consumers
 * (currently the getrandom(2) syscall, which feeds userspace stack
 * canaries and key material).
 *
 * Construction (Linux random.c style, two primitives):
 *
 *   1. EXTRACT/CONDITION: raw hardware entropy (RDSEED/RDRAND/TSC
 *      jitter, biased and low-density) is gathered via the arch
 *      entropy source and conditioned through BLAKE2s-256 into a
 *      uniform 256-bit key.
 *   2. EXPAND/GENERATE: a ChaCha20 stream generator expands that key
 *      into output bytes.  "Fast key erasure" overwrites the key with
 *      the first 32 bytes of each draw, giving backtracking resistance
 *      (a state compromise never reveals previously emitted output).
 *
 * Correctness is checked at boot with the RFC 8439 ChaCha20 and RFC
 * 7693 BLAKE2s known-answer test vectors (csprng_selftest()).
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/base/types.h>

/*
 * Initialise the CSPRNG from hardware entropy.  Idempotent and lazily
 * invoked by csprng_bytes(), but should be called once explicitly at
 * boot (after cpu_init() and hpet_init()) so the first consumer does
 * not pay the entropy-collection cost on a hot path.
 */
void csprng_init(void);

/*
 * Fill `buf` with `len` cryptographically secure random bytes.
 * Safe to call from any context; serialised with an IRQ-save spinlock.
 */
void csprng_bytes(void *buf, size_t len);

/* Convenience: a single 64-bit CSPRNG value. */
uint64_t csprng_u64(void);

/*
 * Return a uniformly distributed value in [0, bound).  Uses rejection
 * sampling to avoid modulo bias.  Returns 0 if bound <= 1.
 */
uint64_t csprng_bounded(uint64_t bound);

/*
 * Return a page-aligned random offset in [0, max_pages * PAGE_SIZE).
 * Used by ASLR to randomize stack and mmap bases.
 */
uint64_t csprng_page_offset(uint64_t max_pages);

/*
 * Run the ChaCha20 and BLAKE2s known-answer tests.  Returns 0 on
 * success, non-zero on mismatch.  Called from csprng_init().
 */
int csprng_selftest(void);

/*
 * Arch-provided raw entropy source.  Fills `buf` with `len` bytes of
 * raw (unconditioned) hardware entropy from the best available source
 * (RDSEED > RDRAND > TSC jitter + HPET).  The bytes are NOT uniform —
 * callers must condition them (e.g. through BLAKE2s) before use as key
 * material.  Implemented in arch/x86_64/entropy.c.
 */
void entropy_collect(void *buf, size_t len);
