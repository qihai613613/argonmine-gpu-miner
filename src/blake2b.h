// Blake2b, one-shot, for the Argon2 reference implementation and the
// Argonmine host-side checks. Public domain / CC0 (same spirit as the
// Argon2 reference code).
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

namespace ab {

static const uint64_t B2B_IV[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
    0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};

static const uint8_t B2B_SIGMA[12][16] = {
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
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3}};

static inline uint64_t rotr64(uint64_t x, unsigned n) {
    return (x >> n) | (x << (64 - n));
}

static inline void b2b_g(uint64_t &a, uint64_t &b, uint64_t &c, uint64_t &d,
                         uint64_t x, uint64_t y) {
    a = a + b + x;
    d = rotr64(d ^ a, 32);
    c = c + d;
    b = rotr64(b ^ c, 24);
    a = a + b + y;
    d = rotr64(d ^ a, 16);
    c = c + d;
    b = rotr64(b ^ c, 63);
}

static void b2b_compress(uint64_t h[8], const uint8_t block[128], uint64_t t,
                         bool last) {
    uint64_t m[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = 0;
        for (int j = 0; j < 8; ++j) m[i] |= (uint64_t)block[i * 8 + j] << (8 * j);
    }
    uint64_t v[16];
    for (int i = 0; i < 8; ++i) {
        v[i] = h[i];
        v[i + 8] = B2B_IV[i];
    }
    v[12] ^= t;
    // v[13] ^= t >> 64; (inputs are far below 2^64 bytes)
    if (last) v[14] = ~v[14];

    for (int r = 0; r < 12; ++r) {
        const uint8_t *s = B2B_SIGMA[r];
        b2b_g(v[0], v[4], v[8], v[12], m[s[0]], m[s[1]]);
        b2b_g(v[1], v[5], v[9], v[13], m[s[2]], m[s[3]]);
        b2b_g(v[2], v[6], v[10], v[14], m[s[4]], m[s[5]]);
        b2b_g(v[3], v[7], v[11], v[15], m[s[6]], m[s[7]]);
        b2b_g(v[0], v[5], v[10], v[15], m[s[8]], m[s[9]]);
        b2b_g(v[1], v[6], v[11], v[12], m[s[10]], m[s[11]]);
        b2b_g(v[2], v[7], v[8], v[13], m[s[12]], m[s[13]]);
        b2b_g(v[3], v[4], v[9], v[14], m[s[14]], m[s[15]]);
    }
    for (int i = 0; i < 8; ++i) h[i] ^= v[i] ^ v[i + 8];
}

// One-shot Blake2b with arbitrary output length up to 64 bytes.
static void blake2b(uint8_t *out, size_t outlen, const uint8_t *in, size_t inlen) {
    uint64_t h[8];
    for (int i = 0; i < 8; ++i) h[i] = B2B_IV[i];
    h[0] ^= 0x01010000ULL ^ (uint64_t)outlen;

    size_t off = 0;
    uint64_t t = 0;
    while (inlen - off > 128) {
        t += 128;
        b2b_compress(h, in + off, t, false);
        off += 128;
    }
    uint8_t buf[128];
    std::memset(buf, 0, sizeof(buf));
    size_t rem = inlen - off;
    if (rem) std::memcpy(buf, in + off, rem);
    t += rem;
    b2b_compress(h, buf, t, true);

    for (size_t i = 0; i < outlen; ++i)
        out[i] = (uint8_t)(h[i >> 3] >> (8 * (i & 7)));
}

// Argon2's H' (variable-length hash), matching the reference blake2b_long().
static void blake2b_long(uint8_t *out, size_t outlen, const uint8_t *in,
                         size_t inlen) {
    if (outlen <= 64) {
        std::vector<uint8_t> buf(4 + inlen);
        buf[0] = (uint8_t)outlen;
        buf[1] = (uint8_t)(outlen >> 8);
        buf[2] = (uint8_t)(outlen >> 16);
        buf[3] = (uint8_t)(outlen >> 24);
        if (inlen) std::memcpy(buf.data() + 4, in, inlen);
        blake2b(out, outlen, buf.data(), buf.size());
        return;
    }

    std::vector<uint8_t> buf(4 + inlen);
    buf[0] = (uint8_t)outlen;
    buf[1] = (uint8_t)(outlen >> 8);
    buf[2] = (uint8_t)(outlen >> 16);
    buf[3] = (uint8_t)(outlen >> 24);
    if (inlen) std::memcpy(buf.data() + 4, in, inlen);

    uint8_t h[64];
    blake2b(h, 64, buf.data(), buf.size());
    size_t remaining = outlen;
    std::memcpy(out, h, 32);
    out += 32;
    remaining -= 32;

    while (remaining > 64) {
        uint8_t tmp[64];
        blake2b(tmp, 64, h, 64);
        std::memcpy(h, tmp, 64);
        std::memcpy(out, h, 32);
        out += 32;
        remaining -= 32;
    }
    if (outlen % 64 != 0) {
        size_t r = ((outlen + 31) / 32) - 2;
        size_t final_len = outlen - 32 * r;
        blake2b(out, final_len, h, 64);
    } else {
        blake2b(out, 64, h, 64);
    }
}

}  // namespace ab
