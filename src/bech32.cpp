#include "bech32.h"

#include <cstring>
#include <vector>

namespace ab {
namespace {

const char *CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

int charset_rev(char c) {
    const char *p = strchr(CHARSET, c);
    return p ? (int)(p - CHARSET) : -1;
}

uint32_t polymod(const std::vector<uint8_t> &values) {
    static const uint32_t GEN[5] = {0x3b6a57b2, 0x26508e6d, 0x1ea119fa,
                                    0x3d4233dd, 0x2a1462b3};
    uint32_t chk = 1;
    for (uint8_t v : values) {
        uint32_t top = chk >> 25;
        chk = ((chk & 0x1ffffff) << 5) ^ v;
        for (int i = 0; i < 5; ++i)
            if ((top >> i) & 1) chk ^= GEN[i];
    }
    return chk;
}

std::vector<uint8_t> hrp_expand(const std::string &hrp) {
    std::vector<uint8_t> v;
    v.reserve(hrp.size() * 2 + 1);
    for (char c : hrp) v.push_back((uint8_t)c >> 5);
    v.push_back(0);
    for (char c : hrp) v.push_back((uint8_t)c & 31);
    return v;
}

bool convert_bits(const std::vector<uint8_t> &in, int frombits, int tobits,
                  bool pad, std::vector<uint8_t> &out) {
    uint32_t acc = 0;
    int bits = 0;
    out.clear();
    for (uint8_t value : in) {
        if (value >> frombits) return false;
        acc = (acc << frombits) | value;
        bits += frombits;
        while (bits >= tobits) {
            bits -= tobits;
            out.push_back((acc >> bits) & ((1 << tobits) - 1));
        }
    }
    if (pad) {
        if (bits) out.push_back((acc << (tobits - bits)) & ((1 << tobits) - 1));
    } else if (bits >= frombits || ((acc << (tobits - bits)) & ((1 << tobits) - 1))) {
        return false;
    }
    return true;
}

std::string encode_raw(const std::string &hrp, const std::vector<uint8_t> &data) {
    std::vector<uint8_t> values = hrp_expand(hrp);
    values.insert(values.end(), data.begin(), data.end());
    values.insert(values.end(), 6, 0);
    uint32_t mod = polymod(values) ^ 0x2bc830a3;
    std::string out = hrp + "1";
    for (uint8_t v : data) out.push_back(CHARSET[v]);
    for (int i = 0; i < 6; ++i)
        out.push_back(CHARSET[(mod >> (5 * (5 - i))) & 31]);
    return out;
}

}  // namespace

bool decode_address(const std::string &address, std::string &hrp, int &version,
                    uint8_t program[32], std::string &err) {
    if (address.size() < 8 || address.size() > 90) {
        err = "address length out of range";
        return false;
    }
    bool has_lower = false, has_upper = false;
    for (char c : address) {
        if (c >= 'a' && c <= 'z') has_lower = true;
        if (c >= 'A' && c <= 'Z') has_upper = true;
    }
    if (has_lower && has_upper) {
        err = "mixed-case address";
        return false;
    }
    std::string a = address;
    for (char &c : a)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');

    size_t pos = a.rfind('1');
    if (pos == std::string::npos || pos == 0 || pos + 7 > a.size()) {
        err = "not a bech32 address";
        return false;
    }
    hrp = a.substr(0, pos);
    if (hrp != "arg") {
        err = "not a mainnet arg address";
        return false;
    }
    std::vector<uint8_t> data;
    for (size_t i = pos + 1; i < a.size(); ++i) {
        int v = charset_rev(a[i]);
        if (v < 0) {
            err = "invalid base32 character";
            return false;
        }
        data.push_back((uint8_t)v);
    }
    std::vector<uint8_t> values = hrp_expand(hrp);
    values.insert(values.end(), data.begin(), data.end());
    if (polymod(values) != 0x2bc830a3) {
        err = "bad bech32m checksum";
        return false;
    }
    data.resize(data.size() - 6);
    if (data.empty()) {
        err = "empty address payload";
        return false;
    }
    version = data[0];
    std::vector<uint8_t> prog;
    std::vector<uint8_t> payload(data.begin() + 1, data.end());
    if (!convert_bits(payload, 5, 8, false, prog)) {
        err = "bad address padding";
        return false;
    }
    if (version != 0 || prog.size() != 32) {
        err = "unsupported address version or length";
        return false;
    }
    memcpy(program, prog.data(), 32);
    return true;
}

std::string encode_address(const std::string &hrp, int version,
                           const uint8_t *program, size_t len) {
    std::vector<uint8_t> payload(program, program + len);
    std::vector<uint8_t> conv;
    convert_bits(payload, 8, 5, true, conv);
    conv.insert(conv.begin(), (uint8_t)version);
    return encode_raw(hrp, conv);
}

}  // namespace ab
