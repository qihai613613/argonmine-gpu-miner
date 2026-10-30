// bech32m decoder/encoder for Argonmine addresses (HRP "arg").
#pragma once
#include <cstdint>
#include <string>

namespace ab {

// Decodes an address, verifies the bech32m checksum, and returns the 32-byte
// witness program. Rejects mixed case, wrong HRP, wrong program length.
bool decode_address(const std::string &addr, std::string &hrp, int &version,
                    uint8_t program[32], std::string &err);

// Encodes a version-0 address (mainly for tests).
std::string encode_address(const std::string &hrp, int version,
                           const uint8_t *program, size_t len);

}  // namespace ab
