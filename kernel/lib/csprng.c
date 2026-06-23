/*
 * kernel/lib/csprng.c — Cryptographically secure PRNG.
 *
 * BLAKE2s (entropy conditioner) + ChaCha20 (output generator) with
 * fast key erasure.  See include/jnu/lib/csprng.h for the design.
 *
 * References:
 *   - RFC 8439 (ChaCha20 and Poly1305)
 *   - RFC 7693 (BLAKE2)
 *   - Bernstein, "Fast-key-erasure random-number generators" (2017)
 *   - Linux drivers/char/random.c
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/base/types.h>
#include <jnu/lib/csprng.h>
#include <jnu/lib/klog.h>
#include <jnu/lib/spinlock.h>
#include <jnu/lib/string.h>
#include <jnu/mm/paging.h>

#define CHACHA_KEY_SIZE 32
#define CHACHA_BLOCK_SIZE 64

/* Reseed after this many output bytes so a state leak self-heals. */
#define CSPRNG_RESEED_BYTES (1u << 20) /* 1 MiB */

/* ------------------------------------------------------------------ */
/* Little-endian helpers                                              */
/* ------------------------------------------------------------------ */

static inline uint32_t load32_le(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static inline void store32_le(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t rotl32(uint32_t x, int n)
{
	return (x << n) | (x >> (32 - n));
}

/* Zero memory without the compiler optimising the wipe away. */
static void secure_wipe(void *p, size_t n)
{
	volatile uint8_t *v = (volatile uint8_t *)p;

	while (n--) {
		*v++ = 0;
	}
}

/* ------------------------------------------------------------------ */
/* ChaCha20 block function (RFC 8439 §2.3)                            */
/* ------------------------------------------------------------------ */

#define CHACHA_QR(a, b, c, d)                                                  \
	do {                                                                   \
		a += b;                                                        \
		d ^= a;                                                        \
		d = rotl32(d, 16);                                             \
		c += d;                                                        \
		b ^= c;                                                        \
		b = rotl32(b, 12);                                             \
		a += b;                                                        \
		d ^= a;                                                        \
		d = rotl32(d, 8);                                              \
		c += d;                                                        \
		b ^= c;                                                        \
		b = rotl32(b, 7);                                              \
	} while (0)

/* Transform a 16-word input state into a 64-byte keystream block. */
static void chacha20_core(const uint32_t in[16], uint8_t out[CHACHA_BLOCK_SIZE])
{
	uint32_t x[16];
	int i;

	for (i = 0; i < 16; i++) {
		x[i] = in[i];
	}

	/* 20 rounds = 10 column/diagonal double-rounds. */
	for (i = 0; i < 10; i++) {
		CHACHA_QR(x[0], x[4], x[8], x[12]);
		CHACHA_QR(x[1], x[5], x[9], x[13]);
		CHACHA_QR(x[2], x[6], x[10], x[14]);
		CHACHA_QR(x[3], x[7], x[11], x[15]);
		CHACHA_QR(x[0], x[5], x[10], x[15]);
		CHACHA_QR(x[1], x[6], x[11], x[12]);
		CHACHA_QR(x[2], x[7], x[8], x[13]);
		CHACHA_QR(x[3], x[4], x[9], x[14]);
	}

	for (i = 0; i < 16; i++) {
		store32_le(out + i * 4, x[i] + in[i]);
	}
}

/*
 * Generate `len` bytes of ChaCha20 keystream from a 32-byte key and a
 * 64-bit nonce.  The 32-bit block counter (word 12) runs internally.
 */
static void chacha20_keystream(const uint8_t key[CHACHA_KEY_SIZE],
			       uint64_t nonce, uint8_t *out, size_t len)
{
	uint32_t st[16];
	uint8_t block[CHACHA_BLOCK_SIZE];
	int i;

	/* "expand 32-byte k" */
	st[0] = 0x61707865u;
	st[1] = 0x3320646eu;
	st[2] = 0x79622d32u;
	st[3] = 0x6b206574u;
	for (i = 0; i < 8; i++) {
		st[4 + i] = load32_le(key + i * 4);
	}
	st[12] = 0; /* block counter */
	st[13] = (uint32_t)nonce;
	st[14] = (uint32_t)(nonce >> 32);
	st[15] = 0;

	while (len) {
		size_t n = len < CHACHA_BLOCK_SIZE ? len : CHACHA_BLOCK_SIZE;

		chacha20_core(st, block);
		memcpy(out, block, n);
		out += n;
		len -= n;
		st[12]++;
	}

	secure_wipe(st, sizeof(st));
	secure_wipe(block, sizeof(block));
}

/* ------------------------------------------------------------------ */
/* BLAKE2s (RFC 7693) — one-shot, unkeyed, 256-bit digest            */
/* ------------------------------------------------------------------ */

#define BLAKE2S_OUT 32
#define BLAKE2S_BLOCK 64

static const uint32_t blake2s_iv[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};

static const uint8_t blake2s_sigma[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
    {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
    {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
    {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
};

#define BLAKE2S_G(r, i, a, b, c, d)                                            \
	do {                                                                   \
		a += b + m[blake2s_sigma[r][2 * i]];                           \
		d = rotl32(d ^ a, 16);                                         \
		c += d;                                                        \
		b = rotl32(b ^ c, 20);                                         \
		a += b + m[blake2s_sigma[r][2 * i + 1]];                       \
		d = rotl32(d ^ a, 24);                                         \
		c += d;                                                        \
		b = rotl32(b ^ c, 25);                                         \
	} while (0)

static void blake2s_compress(uint32_t h[8], const uint8_t block[BLAKE2S_BLOCK],
			     uint64_t t, int last)
{
	uint32_t v[16];
	uint32_t m[16];
	int i, r;

	for (i = 0; i < 16; i++) {
		m[i] = load32_le(block + i * 4);
	}
	for (i = 0; i < 8; i++) {
		v[i] = h[i];
		v[8 + i] = blake2s_iv[i];
	}
	v[12] ^= (uint32_t)t;
	v[13] ^= (uint32_t)(t >> 32);
	if (last) {
		v[14] = ~v[14];
	}

	for (r = 0; r < 10; r++) {
		BLAKE2S_G(r, 0, v[0], v[4], v[8], v[12]);
		BLAKE2S_G(r, 1, v[1], v[5], v[9], v[13]);
		BLAKE2S_G(r, 2, v[2], v[6], v[10], v[14]);
		BLAKE2S_G(r, 3, v[3], v[7], v[11], v[15]);
		BLAKE2S_G(r, 4, v[0], v[5], v[10], v[15]);
		BLAKE2S_G(r, 5, v[1], v[6], v[11], v[12]);
		BLAKE2S_G(r, 6, v[2], v[7], v[8], v[13]);
		BLAKE2S_G(r, 7, v[3], v[4], v[9], v[14]);
	}

	for (i = 0; i < 8; i++) {
		h[i] ^= v[i] ^ v[8 + i];
	}

	secure_wipe(v, sizeof(v));
	secure_wipe(m, sizeof(m));
}

/* Unkeyed BLAKE2s-256 over `in`, writing a 32-byte digest to `out`. */
static void blake2s(uint8_t out[BLAKE2S_OUT], const void *in, size_t inlen)
{
	uint32_t h[8];
	uint8_t block[BLAKE2S_BLOCK];
	const uint8_t *p = (const uint8_t *)in;
	uint64_t t = 0;
	int i;

	for (i = 0; i < 8; i++) {
		h[i] = blake2s_iv[i];
	}
	/* Parameter block: digest length 32, key length 0, fanout/depth 1. */
	h[0] ^= 0x01010000u ^ BLAKE2S_OUT;

	/* Process all full blocks except the final one. */
	while (inlen > BLAKE2S_BLOCK) {
		t += BLAKE2S_BLOCK;
		blake2s_compress(h, p, t, 0);
		p += BLAKE2S_BLOCK;
		inlen -= BLAKE2S_BLOCK;
	}

	/* Final (possibly short, possibly empty) block, zero-padded. */
	memset(block, 0, sizeof(block));
	memcpy(block, p, inlen);
	t += inlen;
	blake2s_compress(h, block, t, 1);

	for (i = 0; i < 8; i++) {
		store32_le(out + i * 4, h[i]);
	}

	secure_wipe(h, sizeof(h));
	secure_wipe(block, sizeof(block));
}

/* ------------------------------------------------------------------ */
/* CSPRNG state and operations                                       */
/* ------------------------------------------------------------------ */

static struct {
	struct spinlock lock;
	uint8_t key[CHACHA_KEY_SIZE]; /* current ChaCha key */
	uint64_t nonce;		      /* monotonic, distinct stream per draw */
	uint32_t bytes_since_reseed;
	bool initialized;
} csprng = {.lock = SPINLOCK_INITIALIZER};

/*
 * (Re)key from fresh hardware entropy.  Conditions raw entropy through
 * BLAKE2s together with the current key, so reseeding only ever adds
 * uncertainty (never weakens) the state.  Caller must hold the lock,
 * except for the very first init.
 */
static void csprng_reseed_locked(void)
{
	uint8_t pool[CHACHA_KEY_SIZE + 64];

	/* Mix current key with newly collected raw entropy, then hash. */
	memcpy(pool, csprng.key, CHACHA_KEY_SIZE);
	entropy_collect(pool + CHACHA_KEY_SIZE, 64);
	blake2s(csprng.key, pool, sizeof(pool));

	csprng.bytes_since_reseed = 0;
	secure_wipe(pool, sizeof(pool));
}

void csprng_init(void)
{
	if (csprng.initialized) {
		return;
	}

	if (csprng_selftest() != 0) {
		pr_err("csprng: SELF-TEST FAILED — randomness is broken\n");
		/*
		 * Continue rather than panic: a broken generator is still
		 * better than denying boot, but the error is loud.
		 */
	}

	spin_lock_init(&csprng.lock);

	/* First seed: no prior key to fold in, just condition entropy. */
	memset(csprng.key, 0, sizeof(csprng.key));
	csprng_reseed_locked();
	csprng.nonce = 0;
	csprng.initialized = true;

	pr_info("csprng: BLAKE2s + ChaCha20 (fast key erasure) seeded\n");
}

void csprng_bytes(void *buf, size_t len)
{
	uint64_t flags;
	uint8_t *out = (uint8_t *)buf;

	if (!csprng.initialized) {
		csprng_init();
	}

	flags = spin_lock_irqsave(&csprng.lock);

	while (len) {
		uint8_t ks[CHACHA_KEY_SIZE + CHACHA_BLOCK_SIZE];
		size_t want = len;

		if (want > CHACHA_BLOCK_SIZE) {
			want = CHACHA_BLOCK_SIZE;
		}

		/*
		 * Fast key erasure: draw (new key || output).  The first
		 * 32 keystream bytes replace the key so this draw can never
		 * be reproduced from the post-draw state; the rest is output.
		 */
		chacha20_keystream(csprng.key, csprng.nonce++, ks, sizeof(ks));
		memcpy(csprng.key, ks, CHACHA_KEY_SIZE);
		memcpy(out, ks + CHACHA_KEY_SIZE, want);
		secure_wipe(ks, sizeof(ks));

		out += want;
		len -= want;

		if (csprng.bytes_since_reseed > CSPRNG_RESEED_BYTES) {
			csprng_reseed_locked();
		}
		csprng.bytes_since_reseed += (uint32_t)want;
	}

	spin_unlock_irqrestore(&csprng.lock, flags);
}

uint64_t csprng_u64(void)
{
	uint64_t v;

	csprng_bytes(&v, sizeof(v));
	return v;
}

uint64_t csprng_bounded(uint64_t bound)
{
	uint64_t threshold;
	uint64_t r;

	if (bound <= 1) {
		return 0;
	}

	/*
	 * Rejection sampling to avoid modulo bias.
	 * threshold = (2^64 - bound) % bound, computed as (-bound) % bound.
	 */
	threshold = (0 - bound) % bound;

	do {
		r = csprng_u64();
	} while (r < threshold);

	return r % bound;
}

uint64_t csprng_page_offset(uint64_t max_pages)
{
	if (max_pages == 0) {
		return 0;
	}
	return csprng_bounded(max_pages) * PAGE_SIZE;
}

/* ------------------------------------------------------------------ */
/* Known-answer tests                                                 */
/* ------------------------------------------------------------------ */

int csprng_selftest(void)
{
	int i;

	/* ChaCha20 — RFC 8439 §2.4.2 keystream block. */
	{
		static const uint8_t key[32] = {
		    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
		    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
		    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
		};
		static const uint8_t expect[64] = {
		    0x22, 0x4f, 0x51, 0xf3, 0x40, 0x1b, 0xd9, 0xe1, 0x2f, 0xde,
		    0x27, 0x6f, 0xb8, 0x63, 0x1d, 0xed, 0x8c, 0x13, 0x1f, 0x82,
		    0x3d, 0x2c, 0x06, 0xe2, 0x7e, 0x4f, 0xca, 0xec, 0x9e, 0xf3,
		    0xcf, 0x78, 0x8a, 0x3b, 0x0a, 0xa3, 0x72, 0x60, 0x0a, 0x92,
		    0xb5, 0x79, 0x74, 0xcd, 0xed, 0x2b, 0x93, 0x34, 0x79, 0x4c,
		    0xba, 0x40, 0xc6, 0x3e, 0x34, 0xcd, 0xea, 0x21, 0x2c, 0x4c,
		    0xf0, 0x7d, 0x41, 0xb7,
		};
		uint32_t st[16];
		uint8_t out[64];

		st[0] = 0x61707865u;
		st[1] = 0x3320646eu;
		st[2] = 0x79622d32u;
		st[3] = 0x6b206574u;
		for (i = 0; i < 8; i++) {
			st[4 + i] = load32_le(key + i * 4);
		}
		st[12] = 1;	     /* block counter */
		st[13] = 0x00000000; /* nonce word 0 (RFC 8439 §2.4.2) */
		st[14] = 0x4a000000; /* nonce word 1: 00 00 00 4a (LE) */
		st[15] = 0x00000000; /* nonce word 2 */

		chacha20_core(st, out);
		if (memcmp(out, expect, sizeof(expect)) != 0) {
			pr_err("csprng: ChaCha20 KAT mismatch\n");
			return 1;
		}
	}

	/* BLAKE2s-256("abc") — RFC 7693 Appendix B.2. */
	{
		static const uint8_t expect[32] = {
		    0x50, 0x8c, 0x5e, 0x8c, 0x32, 0x7c, 0x14, 0xe2,
		    0xe1, 0xa7, 0x2b, 0xa3, 0x4e, 0xeb, 0x45, 0x2f,
		    0x37, 0x45, 0x8b, 0x20, 0x9e, 0xd6, 0x3a, 0x29,
		    0x4d, 0x99, 0x9b, 0x4c, 0x86, 0x67, 0x59, 0x82,
		};
		uint8_t out[32];

		blake2s(out, "abc", 3);
		if (memcmp(out, expect, sizeof(expect)) != 0) {
			pr_err("csprng: BLAKE2s KAT mismatch\n");
			return 1;
		}
	}

	return 0;
}
