// Argon2id reference implementation in portable C++, translated from the
// public reference / Go x/crypto implementation. Used for self-tests and as a
// CPU fallback. Not optimized.
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace ab {

struct Argon2Params {
    uint32_t t = 1;
    uint32_t m = 8192;  // KiB
    uint32_t p = 1;     // lanes
    uint32_t outlen = 32;
};

// Argon2id (type 2), version 0x13, no secret, no associated data.
std::vector<uint8_t> argon2id(const uint8_t *pwd, size_t pwdlen,
                              const uint8_t *salt, size_t saltlen,
                              const Argon2Params &params);

// Argon2's data-independent address blocks for the Argonmine parameters
// (pass 0, lane 0, m=8192 KiB, t=1, Argon2id): 4 slices x 16 blocks x 128
// words. These are constants of the algorithm, not of the header.
void argon2_address_table(uint64_t out[4][16][128]);

}  // namespace ab
