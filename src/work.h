// Argonmine work-protocol parsing and payout verification.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ab {

struct WorkJob {
    std::string id;
    uint8_t prefix[112] = {};
    uint8_t target_be[32] = {};
    uint32_t height = 0;
    std::string network;
    std::string algorithm;
    std::string coinbase_hex;
    std::vector<std::string> merkle_branch;
    std::vector<std::string> witness_branch;
    std::string referrer;
    int referral_pct = 0;
};

bool hex_decode(const std::string &hex, std::vector<uint8_t> &out);
std::string hex_encode(const uint8_t *p, size_t n);

bool parse_work_response(const std::string &body, WorkJob &job, std::string &err);

// Checks that the template pays `payout_program` (32-byte x-only key), commits
// to that coinbase in the header merkle roots, and pays the treasury. Returns
// a human-readable error when something is wrong.
bool verify_work_job(const WorkJob &job, const uint8_t payout_program[32],
                     std::string &err, std::string *warning = nullptr);

double target_difficulty(const uint8_t target_be[32]);

}  // namespace ab
