// Tiny JSON helpers for the flat work-protocol responses. No external deps.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ab {

inline const char *json_skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    return p;
}

// Finds "key" followed by ':' anywhere at the top of the text. The work
// responses are small flat objects, so this is sufficient and robust enough.
inline const char *json_key(const std::string &js, const std::string &key) {
    std::string pat = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = js.find(pat, pos)) != std::string::npos) {
        const char *p = js.c_str() + pos + pat.size();
        const char *end = js.c_str() + js.size();
        p = json_skip_ws(p, end);
        if (p < end && *p == ':') return json_skip_ws(p + 1, end);
        pos += pat.size();
    }
    return nullptr;
}

inline bool json_parse_string(const char *&p, const char *end, std::string &out) {
    out.clear();
    if (p >= end || *p != '"') return false;
    ++p;
    while (p < end) {
        unsigned char c = (unsigned char)*p++;
        if (c == '"') return true;
        if (c == '\\') {
            if (p >= end) return false;
            char e = *p++;
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (end - p < 4) return false;
                    auto hex = [](char h) -> int {
                        if (h >= '0' && h <= '9') return h - '0';
                        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                        return -1;
                    };
                    int v = 0;
                    for (int i = 0; i < 4; ++i) {
                        int d = hex(p[i]);
                        if (d < 0) return false;
                        v = (v << 4) | d;
                    }
                    p += 4;
                    if (v < 0x80) out.push_back((char)v);
                    else if (v < 0x800) {
                        out.push_back((char)(0xC0 | (v >> 6)));
                        out.push_back((char)(0x80 | (v & 0x3F)));
                    } else {
                        out.push_back((char)(0xE0 | (v >> 12)));
                        out.push_back((char)(0x80 | ((v >> 6) & 0x3F)));
                        out.push_back((char)(0x80 | (v & 0x3F)));
                    }
                    break;
                }
                default: return false;
            }
        } else {
            out.push_back((char)c);
        }
    }
    return false;
}

inline bool json_string(const std::string &js, const std::string &key,
                        std::string &out) {
    const char *p = json_key(js, key);
    if (!p) return false;
    const char *end = js.c_str() + js.size();
    p = json_skip_ws(p, end);
    return json_parse_string(p, end, out);
}

inline bool json_u64(const std::string &js, const std::string &key, uint64_t &out) {
    const char *p = json_key(js, key);
    if (!p) return false;
    const char *end = js.c_str() + js.size();
    p = json_skip_ws(p, end);
    if (p >= end || *p < '0' || *p > '9') return false;
    uint64_t v = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        uint64_t d = (uint64_t)(*p - '0');
        if (v > (UINT64_MAX - d) / 10) return false;
        v = v * 10 + d;
        ++p;
    }
    out = v;
    return true;
}

inline bool json_bool(const std::string &js, const std::string &key, bool &out) {
    const char *p = json_key(js, key);
    if (!p) return false;
    const char *end = js.c_str() + js.size();
    if (end - p >= 4 && strncmp(p, "true", 4) == 0) {
        out = true;
        return true;
    }
    if (end - p >= 5 && strncmp(p, "false", 5) == 0) {
        out = false;
        return true;
    }
    return false;
}

inline bool json_string_array(const std::string &js, const std::string &key,
                              std::vector<std::string> &out) {
    out.clear();
    const char *p = json_key(js, key);
    if (!p) return false;
    const char *end = js.c_str() + js.size();
    p = json_skip_ws(p, end);
    if (p >= end || *p != '[') return false;
    ++p;
    p = json_skip_ws(p, end);
    if (p < end && *p == ']') return true;
    while (p < end) {
        std::string s;
        if (!json_parse_string(p, end, s)) return false;
        out.push_back(s);
        p = json_skip_ws(p, end);
        if (p < end && *p == ',') {
            ++p;
            p = json_skip_ws(p, end);
            continue;
        }
        if (p < end && *p == ']') return true;
        return false;
    }
    return false;
}

}  // namespace ab
