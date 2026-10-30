#include "work.h"
#include "bech32.h"
#include "json.h"
#include "sha256.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace ab {
namespace {

constexpr uint64_t MAX_MONEY = 2100000000000000ULL;  // 21e6 * 1e8
const uint8_t TREASURY_KEY[32] = {
    0x7a, 0xd1, 0xa3, 0x0d, 0x48, 0x2a, 0x3f, 0xdb, 0x23, 0xef, 0xd2, 0xde,
    0x4d, 0xa6, 0x9d, 0xe2, 0xe9, 0xdc, 0x50, 0xd2, 0x04, 0xd8, 0xcd, 0x82,
    0x49, 0x51, 0x83, 0xe6, 0xb3, 0xff, 0xdf, 0x6c};

const uint8_t POW_LIMIT[32] = {0x40};

uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

uint64_t rd64le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

bool read_varint(const uint8_t *&p, const uint8_t *end, uint64_t &v, std::string &err) {
    v = 0;
    int shift = 0;
    for (int i = 0; i < 9; ++i) {
        if (p >= end) {
            err = "truncated varint";
            return false;
        }
        uint8_t b = *p++;
        if (i == 8 && (b & 0xfe)) {
            err = "varint overflow";
            return false;
        }
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) {
            if (i > 0 && b == 0) {
                err = "non-minimal varint";
                return false;
            }
            return true;
        }
        shift += 7;
    }
    err = "varint too long";
    return false;
}

int cmp_be(const uint8_t a[32], const uint8_t b[32]) {
    for (int i = 0; i < 32; ++i) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

bool compact_to_target(uint32_t bits, uint8_t out[32]) {
    uint32_t exp = bits >> 24;
    uint32_t mant = bits & 0x007fffff;
    if (bits & 0x00800000) return false;  // sign bit must be zero
    if (mant == 0) return false;
    memset(out, 0, 32);
    int shift = (int)exp - 3;
    for (int i = 0; i < 24; ++i) {
        if (mant & (1u << i)) {
            int bit = i + 8 * shift;
            if (bit < 0 || bit >= 256) return false;
            out[31 - bit / 8] |= (uint8_t)(1u << (bit % 8));
        }
    }
    return true;
}

uint64_t subsidy(uint32_t height) {
    uint32_t halvings = height / 210000;
    if (halvings >= 64) return 0;
    return (5000000000ULL) >> halvings;
}

struct TxOutput {
    int64_t amount = 0;
    uint8_t type = 0;
    std::vector<uint8_t> payload;
};

struct ParsedTx {
    uint32_t version = 0;
    uint32_t lock_height = 0;
    bool coinbase = false;
    std::vector<TxOutput> outputs;
    size_t witness_start = 0;
    uint64_t total_out = 0;
};

bool parse_tx(const std::vector<uint8_t> &raw, ParsedTx &tx, std::string &err) {
    if (raw.size() < 8) {
        err = "coinbase too short";
        return false;
    }
    const uint8_t *base = raw.data();
    const uint8_t *p = base;
    const uint8_t *end = base + raw.size();
    tx.version = rd32le(p);
    p += 4;
    tx.lock_height = rd32le(p);
    p += 4;
    if (tx.version != 1 && tx.version != 2) {
        err = "bad coinbase version";
        return false;
    }

    uint64_t n_in = 0, n_out = 0, n_iss = 0;
    if (!read_varint(p, end, n_in, err) || n_in < 1 || n_in > 10000) {
        if (err.empty()) err = "bad input count";
        return false;
    }
    tx.coinbase = (n_in == 1);
    for (uint64_t i = 0; i < n_in; ++i) {
        if (end - p < 36) {
            err = "truncated input";
            return false;
        }
        if (tx.coinbase) {
            bool zero = true;
            for (int j = 0; j < 32; ++j)
                if (p[j] != 0) zero = false;
            if (!zero || rd32le(p + 32) != 0xffffffffu) tx.coinbase = false;
        }
        p += 36;
    }

    if (!read_varint(p, end, n_out, err) || n_out < 1 || n_out > 10000) {
        if (err.empty()) err = "bad output count";
        return false;
    }
    uint64_t sum = 0;
    for (uint64_t i = 0; i < n_out; ++i) {
        if (end - p < 9) {
            err = "truncated output";
            return false;
        }
        int64_t amount = (int64_t)rd64le(p);
        p += 8;
        uint8_t type = *p++;
        if (amount < 0 || (uint64_t)amount > MAX_MONEY) {
            err = "output amount out of range";
            return false;
        }
        uint64_t plen = 0;
        if (!read_varint(p, end, plen, err)) return false;
        if (plen > 80 || (uint64_t)(end - p) < plen) {
            err = "bad output payload";
            return false;
        }
        TxOutput o;
        o.amount = amount;
        o.type = type;
        o.payload.assign(p, p + plen);
        p += plen;
        if (type == 0 && plen != 32) {
            err = "P2PK payload must be 32 bytes";
            return false;
        }
        if (type == 1 && (plen > 80 || amount != 0)) {
            err = "bad NULLDATA output";
            return false;
        }
        if (type != 0 && type != 1) {
            err = "unknown output type";
            return false;
        }
        if (sum > MAX_MONEY - (uint64_t)amount) {
            err = "coinbase outputs exceed MAX_MONEY";
            return false;
        }
        sum += (uint64_t)amount;
        tx.outputs.push_back(std::move(o));
    }
    tx.total_out = sum;

    if (tx.version == 2) {
        if (!read_varint(p, end, n_iss, err) || n_iss > 1) {
            if (err.empty()) err = "bad issuance count";
            return false;
        }
        if (n_iss == 1) {
            if (end - p < 32 + 32) {
                err = "truncated issuance";
                return false;
            }
            p += 64;
            uint64_t slen = 0;
            if (!read_varint(p, end, slen, err) || slen > 8 ||
                (uint64_t)(end - p) < slen + 8)
                return false;
            p += slen + 8;
        }
    }

    tx.witness_start = (size_t)(p - base);
    uint64_t n_wit = n_in + n_iss;
    for (uint64_t i = 0; i < n_wit; ++i) {
        uint64_t wlen = 0;
        if (!read_varint(p, end, wlen, err)) return false;
        if (wlen > 100 || (uint64_t)(end - p) < wlen) {
            err = "bad witness";
            return false;
        }
        p += wlen;
    }
    if (p != end) {
        err = "trailing bytes after coinbase";
        return false;
    }
    return true;
}

bool merkle_with_branch(const uint8_t txid[32],
                        const std::vector<std::string> &branch, uint8_t root[32],
                        std::string &err) {
    tagged_hash("Argonmine/merkle/leaf", txid, 32, root);
    for (const std::string &s : branch) {
        std::vector<uint8_t> sib;
        if (!hex_decode(s, sib) || sib.size() != 32) {
            err = "bad merkle branch entry";
            return false;
        }
        uint8_t cat[64];
        memcpy(cat, root, 32);
        memcpy(cat + 32, sib.data(), 32);
        tagged_hash("Argonmine/merkle/node", cat, 64, root);
    }
    return true;
}

}  // namespace

bool hex_decode(const std::string &hex, std::vector<uint8_t> &out) {
    if (hex.size() % 2) return false;
    out.resize(hex.size() / 2);
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < out.size(); ++i) {
        int a = val(hex[2 * i]), b = val(hex[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (uint8_t)((a << 4) | b);
    }
    return true;
}

std::string hex_encode(const uint8_t *p, size_t n) {
    static const char *d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 15]);
    }
    return s;
}

bool parse_work_response(const std::string &body, WorkJob &job, std::string &err) {
    if (!json_string(body, "job_id", job.id)) {
        err = "work response has no job_id";
        return false;
    }
    std::string prefix_hex, target_hex;
    std::vector<uint8_t> pbytes, tbytes;
    if (!json_string(body, "header_prefix", prefix_hex) ||
        !hex_decode(prefix_hex, pbytes) || pbytes.size() != 112) {
        err = "bad header_prefix";
        return false;
    }
    memcpy(job.prefix, pbytes.data(), 112);
    if (!json_string(body, "target", target_hex) ||
        !hex_decode(target_hex, tbytes) || tbytes.size() != 32) {
        err = "bad target";
        return false;
    }
    memcpy(job.target_be, tbytes.data(), 32);

    uint64_t h = 0;
    if (json_u64(body, "height", h)) job.height = (uint32_t)h;
    json_string(body, "network", job.network);
    json_string(body, "algorithm", job.algorithm);
    json_string(body, "coinbase", job.coinbase_hex);
    json_string_array(body, "merkle_branch", job.merkle_branch);
    json_string_array(body, "witness_branch", job.witness_branch);
    json_string(body, "referrer", job.referrer);
    uint64_t pct = 0;
    if (json_u64(body, "referral_pct", pct)) job.referral_pct = (int)pct;
    return true;
}

bool verify_work_job(const WorkJob &job, const uint8_t payout_program[32],
                     std::string &err, std::string *warning) {
    if (job.network != "mainnet") {
        err = "work server is not on mainnet";
        return false;
    }
    if (job.algorithm != "argon2id") {
        err = "work server uses an unknown algorithm";
        return false;
    }
    if (rd32le(job.prefix) != 1) {
        err = "template header version is not 1";
        return false;
    }
    uint32_t bits = rd32le(job.prefix + 108);
    uint8_t expected[32];
    if (!compact_to_target(bits, expected) ||
        memcmp(expected, job.target_be, 32) != 0) {
        err = "template target does not match header bits";
        return false;
    }
    if (cmp_be(job.target_be, POW_LIMIT) > 0) {
        err = "template target is easier than the network floor";
        return false;
    }
    bool zero = true;
    for (int i = 0; i < 32; ++i)
        if (job.target_be[i]) zero = false;
    if (zero) {
        err = "template target is zero";
        return false;
    }

    if (job.coinbase_hex.empty()) {
        err = "work response has no coinbase to verify";
        return false;
    }
    std::vector<uint8_t> raw;
    if (!hex_decode(job.coinbase_hex, raw)) {
        err = "coinbase is not valid hex";
        return false;
    }
    ParsedTx tx;
    if (!parse_tx(raw, tx, err)) return false;
    if (!tx.coinbase) {
        err = "template's first transaction is not a coinbase";
        return false;
    }
    if (tx.lock_height != job.height) {
        err = "coinbase lock height does not match the job height";
        return false;
    }

    uint8_t txid[32], wtxid[32];
    tagged_hash("Argonmine/txid", raw.data(), tx.witness_start, txid);
    tagged_hash("Argonmine/wtxid", raw.data(), raw.size(), wtxid);

    uint8_t txroot[32], wroot[32];
    if (!merkle_with_branch(txid, job.merkle_branch, txroot, err)) return false;
    if (!merkle_with_branch(wtxid, job.witness_branch, wroot, err)) return false;
    if (memcmp(txroot, job.prefix + 36, 32) != 0) {
        err = "coinbase does not match the header's transaction root";
        return false;
    }
    if (memcmp(wroot, job.prefix + 68, 32) != 0) {
        err = "coinbase does not match the header's witness root";
        return false;
    }

    uint64_t paid_to_us = 0, paid_to_treasury = 0;
    uint64_t paid_to_ref = 0;
    uint8_t ref_program[32];
    bool have_ref = false;
    if (!job.referrer.empty() && job.referral_pct > 0) {
        std::string hrp, aerr;
        int ver = 0;
        if (!decode_address(job.referrer, hrp, ver, ref_program, aerr)) {
            err = "bad referrer address: " + aerr;
            return false;
        }
        have_ref = true;
    }
    for (const auto &o : tx.outputs) {
        if (o.type != 0 || o.payload.size() != 32) continue;
        if (memcmp(o.payload.data(), payout_program, 32) == 0)
            paid_to_us += (uint64_t)o.amount;
        if (memcmp(o.payload.data(), TREASURY_KEY, 32) == 0)
            paid_to_treasury += (uint64_t)o.amount;
        if (have_ref && memcmp(o.payload.data(), ref_program, 32) == 0)
            paid_to_ref += (uint64_t)o.amount;
    }
    if (paid_to_us == 0) {
        err = "coinbase does not pay the mining address";
        return false;
    }
    uint64_t sub = subsidy(job.height);
    uint64_t need_treasury = (job.height >= 300) ? (sub * 100 / 10000) : 0;
    if (paid_to_treasury < need_treasury) {
        err = "coinbase does not pay the treasury";
        return false;
    }
    if (have_ref && paid_to_ref == 0) {
        err = "coinbase does not pay the accepted referrer";
        return false;
    }
    uint64_t pct = (job.referral_pct > 0) ? (uint64_t)job.referral_pct : 0;
    if (pct > 50) pct = 50;
    // What the coinbase actually distributes after the treasury output. This
    // accepts both the correct split and the explorer's current double
    // deduction of the 1% treasury share (which pays 49 + 0.5 instead of
    // 49.5 + 0.5), while still rejecting a server that pays us less than our
    // agreed share of the template.
    uint64_t distributable =
        (tx.total_out > paid_to_treasury) ? tx.total_out - paid_to_treasury : 0;
    uint64_t expected_miner = distributable * (100 - pct) / 100;
    if (paid_to_us < expected_miner) {
        err = "coinbase pays the mining address less than its share of the template";
        return false;
    }
    if (tx.total_out > sub + MAX_MONEY) {
        err = "coinbase pays more than the subsidy plus fees";
        return false;
    }
    if (warning && tx.total_out < sub) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "template pays only %.8f ARG in total, below the %.8f ARG subsidy; "
                 "the work server appears to deduct the treasury twice",
                 (double)tx.total_out / 1e8, (double)sub / 1e8);
        *warning = buf;
    }
    return true;
}

double target_difficulty(const uint8_t target_be[32]) {
    int first = -1;
    for (int i = 0; i < 32; ++i) {
        if (target_be[i]) {
            first = i;
            break;
        }
    }
    if (first < 0) return 0;
    int n = std::min(8, 32 - first);
    uint64_t mant = 0;
    for (int i = 0; i < n; ++i) mant = (mant << 8) | target_be[first + i];
    int trailing_bits = (32 - first - n) * 8;
    double log2t = std::log2((double)mant) + trailing_bits;
    double lg = 256.0 - log2t;
    if (lg > 120) return 1e30;
    return std::pow(2.0, lg);
}

}  // namespace ab
