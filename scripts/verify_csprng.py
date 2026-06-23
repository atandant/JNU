#!/usr/bin/env python3
"""Temporary verifier for kernel/lib/csprng.c.

Reimplements ChaCha20 (RFC 8439) and the fast-key-erasure draw, and uses
the stdlib BLAKE2s, to confirm the known-answer-test constants compiled
into csprng.c are correct. Throwaway: delete after use.
"""
import hashlib
import struct

MASK = 0xFFFFFFFF


def rotl32(x, n):
    return ((x << n) | (x >> (32 - n))) & MASK


def qr(s, a, b, c, d):
    s[a] = (s[a] + s[b]) & MASK; s[d] = rotl32(s[d] ^ s[a], 16)
    s[c] = (s[c] + s[d]) & MASK; s[b] = rotl32(s[b] ^ s[c], 12)
    s[a] = (s[a] + s[b]) & MASK; s[d] = rotl32(s[d] ^ s[a], 8)
    s[c] = (s[c] + s[d]) & MASK; s[b] = rotl32(s[b] ^ s[c], 7)


def chacha20_core(state):
    x = list(state)
    for _ in range(10):
        qr(x, 0, 4, 8, 12); qr(x, 1, 5, 9, 13)
        qr(x, 2, 6, 10, 14); qr(x, 3, 7, 11, 15)
        qr(x, 0, 5, 10, 15); qr(x, 1, 6, 11, 12)
        qr(x, 2, 7, 8, 13); qr(x, 3, 4, 9, 14)
    out = [(x[i] + state[i]) & MASK for i in range(16)]
    return b"".join(struct.pack("<I", w) for w in out)


def make_state(key, counter, nonce_words):
    st = [0x61707865, 0x3320646e, 0x79622d32, 0x6b206574]
    st += list(struct.unpack("<8I", key))
    st += [counter] + list(nonce_words)
    return st


# --- KAT 1: ChaCha20, RFC 8439 §2.4.2, mirrors csprng_selftest() exactly ---
key = bytes(range(32))
# csprng.c sets st[12]=1, st[13]=0, st[14]=0x4a000000, st[15]=0
state = make_state(key, 1, [0x00000000, 0x4a000000, 0x00000000])
chacha_out = chacha20_core(state)

expect_chacha = bytes([
    0x22, 0x4f, 0x51, 0xf3, 0x40, 0x1b, 0xd9, 0xe1,
    0x2f, 0xde, 0x27, 0x6f, 0xb8, 0x63, 0x1d, 0xed,
    0x8c, 0x13, 0x1f, 0x82, 0x3d, 0x2c, 0x06, 0xe2,
    0x7e, 0x4f, 0xca, 0xec, 0x9e, 0xf3, 0xcf, 0x78,
    0x8a, 0x3b, 0x0a, 0xa3, 0x72, 0x60, 0x0a, 0x92,
    0xb5, 0x79, 0x74, 0xcd, 0xed, 0x2b, 0x93, 0x34,
    0x79, 0x4c, 0xba, 0x40, 0xc6, 0x3e, 0x34, 0xcd,
    0xea, 0x21, 0x2c, 0x4c, 0xf0, 0x7d, 0x41, 0xb7,
])

ok1 = chacha_out == expect_chacha
print(f"ChaCha20 KAT (state from csprng.c): {'PASS' if ok1 else 'FAIL'}")
if not ok1:
    print("  got   ", chacha_out.hex())
    print("  expect", expect_chacha.hex())

# --- KAT 2: BLAKE2s-256("abc"), RFC 7693 B.2, mirrors csprng_selftest() ---
expect_blake = bytes([
    0x50, 0x8c, 0x5e, 0x8c, 0x32, 0x7c, 0x14, 0xe2,
    0xe1, 0xa7, 0x2b, 0xa3, 0x4e, 0xeb, 0x45, 0x2f,
    0x37, 0x45, 0x8b, 0x20, 0x9e, 0xd6, 0x3a, 0x29,
    0x4d, 0x99, 0x9b, 0x4c, 0x86, 0x67, 0x59, 0x82,
])
got_blake = hashlib.blake2s(b"abc", digest_size=32).digest()
ok2 = got_blake == expect_blake
print(f"BLAKE2s KAT (digest_size=32, unkeyed): {'PASS' if ok2 else 'FAIL'}")
if not ok2:
    print("  got   ", got_blake.hex())
    print("  expect", expect_blake.hex())

# --- Sanity: fast-key-erasure draw never reuses key, output != new key ---
def chacha20_keystream(key, nonce, length):
    out = b""
    blk = 0
    while len(out) < length:
        st = make_state(key, blk, [nonce & MASK, (nonce >> 32) & MASK, 0])
        out += chacha20_core(st)
        blk += 1
    return out[:length]


k = bytes(range(32))
nonce = 0
ks = chacha20_keystream(k, nonce, 32 + 64)
new_key, output = ks[:32], ks[32:32 + 64]
ok3 = new_key != k and output != new_key
print(f"Fast-key-erasure (new key != old, output != key): "
      f"{'PASS' if ok3 else 'FAIL'}")

raise SystemExit(0 if (ok1 and ok2 and ok3) else 1)
