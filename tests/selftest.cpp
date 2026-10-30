#include "argon2_cpu.h"
#include "blake2b.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::string hex(const std::vector<uint8_t> &v) {
    static const char *d = "0123456789abcdef";
    std::string s;
    s.reserve(v.size() * 2);
    for (uint8_t b : v) {
        s.push_back(d[b >> 4]);
        s.push_back(d[b & 15]);
    }
    return s;
}

int main() {
    {
        uint8_t out[64];
        ab::blake2b(out, 64, (const uint8_t *)"abc", 3);
        std::vector<uint8_t> v(out, out + 64);
        printf("blake2b-512(abc) = %s\n", hex(v).c_str());
    }
    {
        const char *pwd = "abc";
        const char *salt = "Argonmine/pow/v1";
        ab::Argon2Params p;
        auto out = ab::argon2id((const uint8_t *)pwd, 3, (const uint8_t *)salt,
                                strlen(salt), p);
        printf("argon2id(abc)   = %s\n", hex(out).c_str());
        printf("expected        = 2a37b7f0ffa663244a96a22a77ece9c7cd616aad824a2e8b80db493cefb7a70e\n");
    }
    {
        // A 120-byte header-like input, all zero except a marker.
        std::vector<uint8_t> hdr(120, 0);
        const char *salt = "Argonmine/pow/v1";
        ab::Argon2Params p;
        auto out = ab::argon2id(hdr.data(), hdr.size(), (const uint8_t *)salt,
                                strlen(salt), p);
        printf("argon2id(hdr0)  = %s\n", hex(out).c_str());
    }
    return 0;
}
