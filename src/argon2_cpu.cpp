#include "argon2_cpu.h"
#include "blake2b.h"

#include <cstring>
#include <cstdio>

namespace ab {
namespace {

constexpr uint32_t SYNC_POINTS = 4;
using Block = uint64_t[128];

inline void blamka_g(uint64_t &a, uint64_t &b, uint64_t &c, uint64_t &d) {
    a += b + 2ULL * (uint64_t)(uint32_t)a * (uint64_t)(uint32_t)b;
    d = rotr64(d ^ a, 32);
    c += d + 2ULL * (uint64_t)(uint32_t)c * (uint64_t)(uint32_t)d;
    b = rotr64(b ^ c, 24);
    a += b + 2ULL * (uint64_t)(uint32_t)a * (uint64_t)(uint32_t)b;
    d = rotr64(d ^ a, 16);
    c += d + 2ULL * (uint64_t)(uint32_t)c * (uint64_t)(uint32_t)d;
    b = rotr64(b ^ c, 63);
}

inline void blamka_round(uint64_t *v0, uint64_t *v1, uint64_t *v2, uint64_t *v3,
                         uint64_t *v4, uint64_t *v5, uint64_t *v6, uint64_t *v7,
                         uint64_t *v8, uint64_t *v9, uint64_t *v10, uint64_t *v11,
                         uint64_t *v12, uint64_t *v13, uint64_t *v14, uint64_t *v15) {
    blamka_g(*v0, *v4, *v8, *v12);
    blamka_g(*v1, *v5, *v9, *v13);
    blamka_g(*v2, *v6, *v10, *v14);
    blamka_g(*v3, *v7, *v11, *v15);
    blamka_g(*v0, *v5, *v10, *v15);
    blamka_g(*v1, *v6, *v11, *v12);
    blamka_g(*v2, *v7, *v8, *v13);
    blamka_g(*v3, *v4, *v9, *v14);
}

void process_block(Block &out, const Block &in1, const Block &in2, bool with_xor) {
    Block r, tmp;
    for (int i = 0; i < 128; ++i) r[i] = in1[i] ^ in2[i];
    std::memcpy(tmp, r, sizeof(tmp));

    for (int i = 0; i < 8; ++i)
        blamka_round(&r[16 * i + 0], &r[16 * i + 1], &r[16 * i + 2], &r[16 * i + 3],
                     &r[16 * i + 4], &r[16 * i + 5], &r[16 * i + 6], &r[16 * i + 7],
                     &r[16 * i + 8], &r[16 * i + 9], &r[16 * i + 10], &r[16 * i + 11],
                     &r[16 * i + 12], &r[16 * i + 13], &r[16 * i + 14], &r[16 * i + 15]);

    for (int i = 0; i < 8; ++i)
        blamka_round(&r[2 * i + 0], &r[2 * i + 1], &r[2 * i + 16], &r[2 * i + 17],
                     &r[2 * i + 32], &r[2 * i + 33], &r[2 * i + 48], &r[2 * i + 49],
                     &r[2 * i + 64], &r[2 * i + 65], &r[2 * i + 80], &r[2 * i + 81],
                     &r[2 * i + 96], &r[2 * i + 97], &r[2 * i + 112], &r[2 * i + 113]);

    if (with_xor) {
        for (int i = 0; i < 128; ++i) out[i] ^= in1[i] ^ in2[i] ^ r[i];
    } else {
        for (int i = 0; i < 128; ++i) out[i] = in1[i] ^ in2[i] ^ r[i];
    }
}

uint32_t index_alpha(uint64_t rand, uint32_t lanes, uint32_t segments,
                     uint32_t threads, uint32_t pass, uint32_t slice,
                     uint32_t lane, uint32_t index) {
    uint32_t ref_lane = (uint32_t)(rand >> 32) % threads;
    if (pass == 0 && slice == 0) ref_lane = lane;

    uint64_t m = 3 * (uint64_t)segments;
    uint64_t s = ((slice + 1) % SYNC_POINTS) * (uint64_t)segments;
    if (lane == ref_lane) m += index;
    if (pass == 0) {
        m = (uint64_t)slice * segments;
        s = 0;
        if (slice == 0 || lane == ref_lane) m += index;
    }
    if (index == 0 || lane == ref_lane) m--;

    uint64_t p = rand & 0xFFFFFFFFu;
    p = (p * p) >> 32;
    p = (p * m) >> 32;
    return (uint32_t)(lane * lanes +
                      (uint32_t)((s + m - (p + 1)) % (uint64_t)lanes));
}

}  // namespace

std::vector<uint8_t> argon2id(const uint8_t *pwd, size_t pwdlen,
                              const uint8_t *salt, size_t saltlen,
                              const Argon2Params &params) {
    const uint32_t lanes = params.p;
    uint32_t memory = params.m;
    memory = memory / (SYNC_POINTS * lanes) * (SYNC_POINTS * lanes);
    if (memory < 2 * SYNC_POINTS * lanes) memory = 2 * SYNC_POINTS * lanes;
    const uint32_t segments = memory / lanes / SYNC_POINTS;

    // H0 = Blake2b-512(LE32(p) || LE32(outlen) || LE32(m) || LE32(t) ||
    //                  LE32(version) || LE32(type) || LE32(pwdlen) || pwd ||
    //                  LE32(saltlen) || salt || LE32(0) || LE32(0))
    std::vector<uint8_t> h0in;
    h0in.reserve(24 + 4 + pwdlen + 4 + saltlen + 8);
    auto put32 = [&](uint32_t v) {
        h0in.push_back((uint8_t)v);
        h0in.push_back((uint8_t)(v >> 8));
        h0in.push_back((uint8_t)(v >> 16));
        h0in.push_back((uint8_t)(v >> 24));
    };
    put32(lanes);
    put32(params.outlen);
    put32(memory);
    put32(params.t);
    put32(0x13);
    put32(2);  // Argon2id
    put32((uint32_t)pwdlen);
    h0in.insert(h0in.end(), pwd, pwd + pwdlen);
    put32((uint32_t)saltlen);
    h0in.insert(h0in.end(), salt, salt + saltlen);
    put32(0);
    put32(0);

    uint8_t h0[64];
    blake2b(h0, 64, h0in.data(), h0in.size());

    std::vector<Block> B(memory);
    std::vector<uint8_t> zero_bytes(1024, 0);
    Block zero;
    std::memcpy(zero, zero_bytes.data(), 1024);

    // B[0] and B[1] per lane.
    for (uint32_t lane = 0; lane < lanes; ++lane) {
        uint8_t seed[72];
        std::memcpy(seed, h0, 64);
        uint32_t j = lane * (memory / lanes);
        for (uint32_t idx = 0; idx < 2; ++idx) {
            seed[64] = (uint8_t)idx;
            seed[65] = (uint8_t)(idx >> 8);
            seed[66] = (uint8_t)(idx >> 16);
            seed[67] = (uint8_t)(idx >> 24);
            seed[68] = (uint8_t)lane;
            seed[69] = (uint8_t)(lane >> 8);
            seed[70] = (uint8_t)(lane >> 16);
            seed[71] = (uint8_t)(lane >> 24);
            uint8_t out[1024];
            blake2b_long(out, 1024, seed, sizeof(seed));
            std::memcpy(B[j + idx], out, 1024);
        }
    }

    for (uint32_t pass = 0; pass < params.t; ++pass) {
        for (uint32_t slice = 0; slice < SYNC_POINTS; ++slice) {
            for (uint32_t lane = 0; lane < lanes; ++lane) {
                bool di = (pass == 0 && slice < SYNC_POINTS / 2);
                Block addresses, input;
                std::memset(&addresses, 0, sizeof(addresses));
                std::memset(&input, 0, sizeof(input));
                if (di) {
                    input[0] = pass;
                    input[1] = lane;
                    input[2] = slice;
                    input[3] = memory;
                    input[4] = params.t;
                    input[5] = 2;
                }

                uint32_t index = (pass == 0 && slice == 0) ? 2 : 0;
                uint32_t offset = lane * (memory / lanes) + slice * segments + index;
                if (index == 2) {
                    input[6]++;
                    process_block(addresses, input, zero, false);
                    process_block(addresses, addresses, zero, false);
                }

                for (; index < segments; ++index, ++offset) {
                    uint32_t prev = offset - 1;  // index 0 never happens at slice 0
                    uint64_t random;
                    if (di) {
                        if (index % 128 == 0) {
                            input[6]++;
                            process_block(addresses, input, zero, false);
                            process_block(addresses, addresses, zero, false);
                        }
                        random = addresses[index % 128];
                    } else {
                        random = B[prev][0];
                    }
                    uint32_t ref = index_alpha(random, memory / lanes, segments,
                                               lanes, pass, slice, lane, index);
                    process_block(B[offset], B[prev], B[ref], pass > 0);
                }
            }
        }
    }

    Block last;
    std::memcpy(last, B[memory - 1], sizeof(last));
    for (uint32_t lane = 0; lane + 1 < lanes; ++lane) {
        const Block &b = B[(lane + 1) * (memory / lanes) - 1];
        for (int i = 0; i < 128; ++i) last[i] ^= b[i];
    }

    std::vector<uint8_t> key(params.outlen);
    // blake2b_long(key, outlen, last as 1024 little-endian bytes)
    uint8_t last_bytes[1024];
    for (int i = 0; i < 128; ++i)
        for (int j = 0; j < 8; ++j)
            last_bytes[i * 8 + j] = (uint8_t)(last[i] >> (8 * j));
    blake2b_long(key.data(), key.size(), last_bytes, sizeof(last_bytes));
    return key;
}

void argon2_address_table(uint64_t out[4][16][128]) {
    Block zero, input, addresses;
    std::memset(&zero, 0, sizeof(zero));
    for (uint32_t slice = 0; slice < SYNC_POINTS; ++slice) {
        for (uint32_t counter = 1; counter <= 16; ++counter) {
            std::memset(&input, 0, sizeof(input));
            input[0] = 0;      // pass
            input[1] = 0;      // lane
            input[2] = slice;  // slice
            input[3] = 8192;   // memory blocks
            input[4] = 1;      // passes
            input[5] = 2;      // Argon2id
            input[6] = counter;
            std::memset(&addresses, 0, sizeof(addresses));
            process_block(addresses, input, zero, false);
            process_block(addresses, addresses, zero, false);
            std::memcpy(out[slice][counter - 1], addresses, sizeof(Block));
        }
    }
}

}  // namespace ab
