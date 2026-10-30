#include "gpu_miner.h"
#include "argon2_cpu.h"
#include "bech32.h"
#include "http.h"
#include "json.h"
#include "work.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

volatile LONG g_stop = 0;

BOOL WINAPI ctrl_handler(DWORD) {
    InterlockedExchange(&g_stop, 1);
    return TRUE;
}

using Clock = std::chrono::steady_clock;

std::string fmt_rate(double h) {
    char buf[64];
    if (h >= 1e6) snprintf(buf, sizeof(buf), "%.2f MH/s", h / 1e6);
    else if (h >= 1e3) snprintf(buf, sizeof(buf), "%.2f kH/s", h / 1e3);
    else snprintf(buf, sizeof(buf), "%.1f H/s", h);
    return buf;
}

std::string fmt_duration(double s) {
    char buf[64];
    if (!(s >= 0) || s > 1e12) return "a very long time";
    if (s < 90) snprintf(buf, sizeof(buf), "%.0f s", s);
    else if (s < 5400) snprintf(buf, sizeof(buf), "%.1f min", s / 60);
    else if (s < 172800) snprintf(buf, sizeof(buf), "%.1f h", s / 3600);
    else snprintf(buf, sizeof(buf), "%.1f days", s / 86400);
    return buf;
}

std::string fmt_count(double n) {
    char buf[64];
    if (n >= 1e12) snprintf(buf, sizeof(buf), "%.2fT", n / 1e12);
    else if (n >= 1e9) snprintf(buf, sizeof(buf), "%.2fG", n / 1e9);
    else if (n >= 1e6) snprintf(buf, sizeof(buf), "%.2fM", n / 1e6);
    else if (n >= 1e3) snprintf(buf, sizeof(buf), "%.2fK", n / 1e3);
    else snprintf(buf, sizeof(buf), "%.0f", n);
    return buf;
}

uint64_t random_u64() {
    static std::mt19937_64 rng([] {
        std::random_device rd;
        uint64_t seed = ((uint64_t)rd() << 32) ^ rd();
        seed ^= (uint64_t)std::chrono::high_resolution_clock::now()
                    .time_since_epoch()
                    .count();
        return seed;
    }());
    return rng();
}

void trim(std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) {
        s.clear();
        return;
    }
    s = s.substr(a, b - a + 1);
}

std::string exe_dir() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (!n) return ".";
    std::string s(buf, n);
    size_t p = s.find_last_of("\\/");
    return p == std::string::npos ? "." : s.substr(0, p);
}

std::string config_path() { return exe_dir() + "\\argonmine-gpu.cfg"; }

bool read_saved_config(std::string &addr, int &batch) {
    std::ifstream f(config_path());
    if (!f) return false;
    std::getline(f, addr);
    trim(addr);
    batch = 0;
    std::string line;
    while (std::getline(f, line)) {
        trim(line);
        if (line.rfind("batch=", 0) == 0) {
            int n = atoi(line.c_str() + 6);
            if (n > 0) batch = n;
        }
    }
    return !addr.empty();
}

bool save_config(const std::string &addr, int batch) {
    std::ofstream f(config_path(), std::ios::trunc);
    if (!f) return false;
    f << addr << "\nbatch=" << batch << "\n";
    return true;
}

bool valid_address(const std::string &addr) {
    std::string hrp, err;
    int ver = 0;
    uint8_t program[32];
    return ab::decode_address(addr, hrp, ver, program, err);
}

std::string ask_address() {
    for (;;) {
        printf("Enter your ARG address (arg1...): ");
        std::string line;
        if (!std::getline(std::cin, line)) return "";
        trim(line);
        if (line.empty()) {
            printf("No address entered.\n");
            continue;
        }
        for (char &c : line)
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (valid_address(line)) return line;
        printf("Invalid address. Check the address on app.argonmine.com -> Receive.\n");
    }
}

int ask_batch(int def) {
    printf("Batch (hashes in flight, 8 MiB VRAM each; 1024 = 8 GiB) [%d]: ", def);
    std::string line;
    if (!std::getline(std::cin, line)) return def;
    trim(line);
    if (line.empty()) return def;
    int n = atoi(line.c_str());
    if (n < 16 || n > 4096) {
        printf("Keeping %d (valid range 16-4096).\n", def);
        return def;
    }
    return (n / 16) * 16;
}

std::string json_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

std::string status_path() { return exe_dir() + "\\miner-status.json"; }

void write_status(bool online, const std::string &gpu, int batch, double hashrate,
                  uint64_t total_hashes, uint64_t accepted, uint64_t solutions,
                  uint64_t stale, uint64_t rejected, uint32_t height,
                  const std::string &job_id, double expected_hashes, double eta,
                  const std::string &last_error, double uptime_seconds) {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    std::ostringstream os;
    os << "{";
    os << "\"online\":" << (online ? "true" : "false");
    os << ",\"timestamp_ms\":" << ms;
    os << ",\"uptime_seconds\":" << uptime_seconds;
    os << ",\"gpu\":\"" << json_escape(gpu) << "\"";
    os << ",\"batch\":" << batch;
    os << ",\"hashrate\":" << hashrate;
    os << ",\"total_hashes\":" << total_hashes;
    os << ",\"height\":" << height;
    os << ",\"job_id\":\"" << json_escape(job_id) << "\"";
    os << ",\"expected_hashes\":" << expected_hashes;
    os << ",\"eta_seconds\":" << eta;
    os << ",\"accepted_blocks\":" << accepted;
    os << ",\"solutions\":" << solutions;
    os << ",\"stale\":" << stale;
    os << ",\"rejected\":" << rejected;
    os << ",\"last_error\":\"" << json_escape(last_error) << "\"";
    os << "}\n";
    std::string path = status_path();
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return;
        f << os.str();
    }
    MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void print_usage() {
    printf(
        "argonmine-gpu - native GPU miner for Argonmine (ARG)\n"
        "\n"
        "Usage: argonmine-gpu --address arg1... [options]\n"
        "\n"
        "  --address ADDR    payout address (bech32m, arg1...)\n"
        "  --ref ADDR        optional referrer address\n"
        "  --url URL         work server (default https://explorer.argonmine.com)\n"
        "  --batch N         hashes in flight; each uses 8 MiB of VRAM\n"
        "                    (default 1024 = 8 GiB; auto-reduced if needed)\n"
        "  --device N        CUDA device index (default 0)\n"
        "  --no-verify       do not verify the coinbase/merkle roots\n"
        "  --dry-run         fetch and mine, but never submit a block\n"
        "  --test-submit     fetch one job, submit a dummy nonce, print the reply\n"
        "  --duration N      stop after N seconds (0 = until Ctrl+C)\n"
        "  --benchmark [S]   benchmark for S seconds (default 10), no network\n"
        "  --selftest        cross-check the GPU against the CPU reference\n"
        "  --help            this text\n");
}

bool run_selftest(ab::GpuMiner &gpu) {
    printf("Self-test: comparing GPU Argon2id with the CPU reference...\n");
    std::mt19937_64 rng(0x123456789abcdef0ULL);
    bool ok = true;
    for (int i = 0; i < 4; ++i) {
        uint8_t hdr[120];
        for (int j = 0; j < 120; j += 8) {
            uint64_t v = rng();
            memcpy(hdr + j, &v, 8);
        }
        ab::Argon2Params p;
        auto cpu = ab::argon2id(hdr, sizeof(hdr),
                                (const uint8_t *)"Argonmine/pow/v1", 16, p);
        uint8_t tag[32];
        std::string err;
        uint64_t nonce;
        memcpy(&nonce, hdr + 112, 8);
        if (!gpu.hash_one(hdr, nonce, tag, err)) {
            printf("  GPU error: %s\n", err.c_str());
            return false;
        }
        bool same = memcmp(cpu.data(), tag, 32) == 0;
        ok = ok && same;
        printf("  header %d: %s  %s\n", i, same ? "OK" : "MISMATCH",
               ab::hex_encode(tag, 32).c_str());
    }
    // Mainnet block 652, a real solved block. This checks the exact header
    // serialization, the Argon2 tag and the big-endian target comparison.
    {
        const char *prefix_hex =
            "01000000d13d3f9f66e06153ce20032cd91a3b99f268960a1a0f07dda591fbeb97ab1d08"
            "251ae009306a07e78d9a9e94271d3959a0128918323bb01b325c5a32766da5f9"
            "9691b85e7afce56e56c73093557596462c7d110a95abc94648c3c58989a6912d"
            "5874e46a00000000a8bc0a1e";
        const char *target_hex =
            "00000abca8000000000000000000000000000000000000000000000000000000";
        const char *pow_hex =
            "51ac1255085b5f48c19f8f77ff39dca4937cfa70c66825aeae8dc13eea030000";
        const uint64_t nonce = 16350053624604184909ULL;
        std::vector<uint8_t> p, t, e;
        bool vec_ok = ab::hex_decode(prefix_hex, p) && p.size() == 112 &&
                      ab::hex_decode(target_hex, t) && t.size() == 32 &&
                      ab::hex_decode(pow_hex, e) && e.size() == 32;
        if (vec_ok) {
            uint8_t prefix[112], target[32], expected[32], tag[32];
            memcpy(prefix, p.data(), 112);
            memcpy(target, t.data(), 32);
            memcpy(expected, e.data(), 32);
            std::string verr;
            bool hash_ok = gpu.hash_one(prefix, nonce, tag, verr) &&
                           memcmp(tag, expected, 32) == 0;
            gpu.set_job(prefix, target, verr);
            std::vector<ab::GpuFound> found;
            double sec = 0;
            bool mine_ok = gpu.mine(nonce, found, &sec, verr);
            bool hit = false;
            for (const auto &f : found)
                if (f.nonce == nonce) hit = true;
            printf("  mainnet block 652: hash %s, target %s\n",
                   hash_ok ? "OK" : "MISMATCH", hit ? "OK" : "MISMATCH");
            vec_ok = hash_ok && mine_ok && hit;
        } else {
            printf("  mainnet block 652: BAD TEST VECTOR\n");
        }
        ok = ok && vec_ok;
    }
    printf("%s\n", ok ? "Self-test passed." : "SELF-TEST FAILED.");
    return ok;
}

int benchmark(ab::GpuMiner &gpu, int seconds) {
    uint8_t prefix[112];
    for (int i = 0; i < 112; i += 8) {
        uint64_t v = random_u64();
        memcpy(prefix + i, &v, 8);
    }
    uint8_t target[32] = {};
    std::string err;
    gpu.set_job(prefix, target, err);
    std::vector<ab::GpuFound> found;
    double sec = 0;
    uint64_t base = random_u64();
    // Warm up.
    gpu.mine(base, found, &sec, err);
    base += gpu.batch();
    found.clear();

    auto start = Clock::now();
    auto last_print = start;
    uint64_t hashes = 0;
    double best = 1e9, total = 0;
    int runs = 0;
    while (std::chrono::duration<double>(Clock::now() - start).count() < seconds) {
        found.clear();
        if (!gpu.mine(base, found, &sec, err)) {
            printf("GPU error: %s\n", err.c_str());
            return 1;
        }
        base += gpu.batch();
        hashes += gpu.batch();
        runs++;
        total += sec;
        if (sec < best) best = sec;
        auto now = Clock::now();
        double elapsed = std::chrono::duration<double>(now - start).count();
        if (std::chrono::duration<double>(now - last_print).count() >= 0.5 ||
            elapsed >= seconds) {
            last_print = now;
            printf("\r  %s  |  %llu hashes  |  %.1fs",
                   fmt_rate(hashes / elapsed).c_str(),
                   (unsigned long long)hashes, elapsed);
            fflush(stdout);
        }
    }
    printf("\n");
    printf("Batch %d, %d runs: avg %.1f H/s, best %.1f H/s\n", gpu.batch(), runs,
           (double)hashes / total, (double)gpu.batch() / best);
    return 0;
}

struct Options {
    std::string address;
    std::string ref;
    std::string url = "https://explorer.argonmine.com";
    int batch = 1024;
    bool batch_set = false;
    int device = 0;
    bool verify = true;
    bool selftest = false;
    bool bench = false;
    bool dry_run = false;
    bool test_submit = false;
    bool help = false;
    int bench_seconds = 10;
    int duration = 0;
};

bool parse_args(int argc, char **argv, Options &o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](std::string &out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        if (a == "--address") {
            if (!next(o.address)) return false;
        } else if (a == "--ref") {
            if (!next(o.ref)) return false;
        } else if (a == "--url") {
            if (!next(o.url)) return false;
        } else if (a == "--batch") {
            std::string s;
            if (!next(s)) return false;
            o.batch = atoi(s.c_str());
            o.batch_set = true;
        } else if (a == "--device") {
            std::string s;
            if (!next(s)) return false;
            o.device = atoi(s.c_str());
        } else if (a == "--no-verify") {
            o.verify = false;
        } else if (a == "--dry-run") {
            o.dry_run = true;
        } else if (a == "--test-submit") {
            o.test_submit = true;
        } else if (a == "--duration") {
            std::string s;
            if (!next(s)) return false;
            o.duration = atoi(s.c_str());
        } else if (a == "--selftest") {
            o.selftest = true;
        } else if (a == "--benchmark") {
            o.bench = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                o.bench_seconds = atoi(argv[++i]);
                if (o.bench_seconds < 1) o.bench_seconds = 1;
            }
        } else if (a == "--help" || a == "-h") {
            o.help = true;
            return true;
        } else {
            printf("Unknown option: %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

bool submit(ab::WorkJob &job, const std::string &url, uint64_t nonce,
            const std::string &address, ab::HttpResponse &resp, std::string &err) {
    std::string body = "{\"job_id\":\"" + job.id + "\",\"nonce\":" +
                       std::to_string(nonce) + ",\"address\":\"" + address + "\"}";
    if (!ab::http_request(url + "/api/work/submit", "POST", body,
                          "application/json", resp)) {
        err = resp.error;
        return false;
    }
    if (resp.status != 200) {
        err = "submit HTTP " + std::to_string(resp.status) + ": " + resp.body;
        return false;
    }
    return true;
}

}  // namespace

int run_miner(int argc, char **argv, bool interactive) {
    Options o;
    if (!parse_args(argc, argv, o)) {
        print_usage();
        return 1;
    }
    if (o.help) {
        print_usage();
        return 0;
    }

    if (interactive && o.address.empty()) {
        std::string saved;
        int saved_batch = 0;
        if (read_saved_config(saved, saved_batch)) {
            for (char &c : saved)
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (valid_address(saved)) {
                o.address = saved;
                if (!o.batch_set && saved_batch >= 16) o.batch = saved_batch;
                printf("Using saved address: %s (batch %d)\n", o.address.c_str(),
                       o.batch);
            } else {
                printf("Saved address is invalid; please enter a new one.\n");
            }
        }
        if (o.address.empty()) {
            std::string a = ask_address();
            if (a.empty()) return 1;
            o.address = a;
            if (!o.batch_set) o.batch = ask_batch(o.batch);
            if (save_config(a, o.batch))
                printf("Saved to %s\n", config_path().c_str());
        }
        printf("\n");
    }

    ab::GpuMiner gpu;
    std::string err;
    printf("Argonmine GPU miner\n");
    if (!gpu.init(o.device, o.batch, err)) {
        printf("GPU init failed: %s\n", err.c_str());
        return 1;
    }
    auto info = gpu.info();
    printf("GPU: %s (sm_%d%d, %d SMs, %.1f GiB)\n", info.name.c_str(), info.major,
           info.minor, info.sm_count, info.total_mem / 1073741824.0);
    printf("Batch: %d hashes = %.2f GiB of VRAM\n", gpu.batch(),
           gpu.memory_bytes() / 1073741824.0);

    if (o.selftest) {
        bool ok = run_selftest(gpu);
        return ok ? 0 : 2;
    }
    if (o.bench) return benchmark(gpu, o.bench_seconds);

    if (o.address.empty()) {
        printf("--address is required (or use --benchmark / --selftest)\n");
        return 1;
    }

    std::string hrp, aerr;
    int ver = 0;
    uint8_t payout[32];
    std::string address = o.address;
    for (char &c : address)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (!ab::decode_address(address, hrp, ver, payout, aerr)) {
        printf("Bad address: %s\n", aerr.c_str());
        return 1;
    }
    if (!o.ref.empty()) {
        for (char &c : o.ref)
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        uint8_t rp[32];
        if (!ab::decode_address(o.ref, hrp, ver, rp, aerr)) {
            printf("Bad referrer address: %s\n", aerr.c_str());
            return 1;
        }
        if (o.ref == address) {
            printf("The referrer cannot be the mining address.\n");
            return 1;
        }
    }
    printf("Payout: %s\n", address.c_str());
    if (!o.ref.empty()) printf("Referrer: %s\n", o.ref.c_str());
    printf("Work server: %s\n", o.url.c_str());
    printf("Verification: %s\n", o.verify ? "on" : "off");

    if (o.test_submit) {
        std::string q = o.url + "/api/work?address=" + ab::url_encode(address);
        if (!o.ref.empty()) q += "&ref=" + ab::url_encode(o.ref);
        ab::HttpResponse resp;
        if (!ab::http_request(q, "GET", "", "", resp) || resp.status != 200) {
            printf("GET failed: %s\n", resp.error.c_str());
            return 1;
        }
        ab::WorkJob job;
        if (!ab::parse_work_response(resp.body, job, err)) {
            printf("parse failed: %s\n", err.c_str());
            return 1;
        }
        std::string warning;
        if (o.verify && !ab::verify_work_job(job, payout, err, &warning)) {
            printf("verification failed: %s\n", err.c_str());
            return 1;
        }
        if (!warning.empty()) printf("warning: %s\n", warning.c_str());
        // Submit with a different (valid) address so the server rejects it
        // before doing any proof-of-work check: this tests the POST path
        // without any chance of finding a real block.
        uint8_t dummy_prog[32];
        for (int i = 0; i < 32; ++i) dummy_prog[i] = (uint8_t)(i * 37 + 11);
        std::string dummy = ab::encode_address("arg", 0, dummy_prog, 32);
        if (dummy == address) {
            dummy_prog[0] ^= 1;
            dummy = ab::encode_address("arg", 0, dummy_prog, 32);
        }
        ab::HttpResponse sr;
        std::string serr;
        if (!submit(job, o.url, 1, dummy, sr, serr)) {
            printf("submit failed: %s\n", serr.c_str());
            return 1;
        }
        bool accepted = false, stale = false;
        std::string reason;
        ab::json_bool(sr.body, "accepted", accepted);
        ab::json_bool(sr.body, "stale", stale);
        ab::json_string(sr.body, "reason", reason);
        printf("submit HTTP %d: %s\n", sr.status, sr.body.c_str());
        printf("accepted=%d stale=%d reason=%s\n", (int)accepted, (int)stale,
               reason.c_str());
        return 0;
    }

    printf("Press Ctrl+C to stop.\n\n");

    uint64_t total_hashes = 0;
    uint64_t blocks = 0;
    uint64_t solutions = 0;
    uint64_t stale_count = 0;
    uint64_t rejected_count = 0;
    int gpu_errors = 0;
    double rate = 0;
    auto rate_time = Clock::now();
    auto run_start = Clock::now();
    auto last_status_write = run_start;
    uint64_t rate_hashes = 0;
    std::string last_error;
    bool warned_template = false;

    auto write_now = [&](bool online, double hr, uint32_t h,
                         const std::string &jid, double exp, double eta,
                         const std::string &e) {
        write_status(online, info.name, gpu.batch(), hr, total_hashes, blocks,
                     solutions, stale_count, rejected_count, h, jid, exp, eta, e,
                     std::chrono::duration<double>(Clock::now() - run_start)
                         .count());
    };
    write_now(true, 0, 0, "", 0, -1, "");

    while (!g_stop && (o.duration <= 0 ||
                       std::chrono::duration<double>(Clock::now() - run_start).count() <
                           o.duration)) {
        std::string q = o.url + "/api/work?address=" + ab::url_encode(address);
        if (!o.ref.empty()) q += "&ref=" + ab::url_encode(o.ref);
        ab::HttpResponse resp;
        if (!ab::http_request(q, "GET", "", "", resp) || resp.status != 200) {
            std::string msg = resp.error;
            if (msg.empty())
                msg = "HTTP " + std::to_string(resp.status) + ": " + resp.body;
            printf("work: %s\n", msg.c_str());
            last_error = msg;
            write_now(true, 0, 0, "", 0, -1, last_error);
            for (int i = 0; i < 30 && !g_stop; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        ab::WorkJob job;
        if (!ab::parse_work_response(resp.body, job, err)) {
            printf("work: %s\n", err.c_str());
            last_error = err;
            write_now(true, 0, 0, "", 0, -1, last_error);
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }
        std::string warning;
        if (o.verify && !ab::verify_work_job(job, payout, err, &warning)) {
            printf("Refusing job %s: %s\n", job.id.c_str(), err.c_str());
            last_error = err;
            write_now(true, 0, 0, "", 0, -1, last_error);
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        last_error.clear();
        if (!warning.empty() && !warned_template) {
            printf("warning: %s\n", warning.c_str());
            warned_template = true;
        }
        gpu.set_job(job.prefix, job.target_be, err);

        double expected_hashes = ab::target_difficulty(job.target_be);
        uint64_t base = random_u64();
        auto job_start = Clock::now();
        auto last_status = job_start;
        bool advance = false;

        while (!g_stop && !advance) {
            double since = std::chrono::duration<double>(Clock::now() - job_start).count();
            if (since >= 20.0) break;
            if (o.duration > 0 &&
                std::chrono::duration<double>(Clock::now() - run_start).count() >=
                    o.duration)
                break;

            std::vector<ab::GpuFound> found;
            double sec = 0;
            if (!gpu.mine(base, found, &sec, err)) {
                printf("GPU error: %s\n", err.c_str());
                last_error = err;
                if (++gpu_errors >= 3) {
                    printf("GPU failed 3 times in a row; exiting so a watchdog can restart it.\n");
                    write_now(false, rate, 0, "", 0, -1, last_error);
                    return 1;
                }
                std::this_thread::sleep_for(std::chrono::seconds(2));
                advance = true;
                break;
            }
            gpu_errors = 0;
            last_error.clear();
            base += (uint64_t)gpu.batch();
            total_hashes += gpu.batch();
            rate_hashes += gpu.batch();

            auto now = Clock::now();
            double dt = std::chrono::duration<double>(now - rate_time).count();
            if (dt >= 1.0) {
                double inst = (double)rate_hashes / dt;
                rate = (rate == 0) ? inst : rate * 0.7 + inst * 0.3;
                rate_time = now;
                rate_hashes = 0;
            }
            if (std::chrono::duration<double>(now - last_status_write).count() >= 1.0) {
                double eta = (rate > 1 && expected_hashes > 0)
                                 ? expected_hashes / rate
                                 : -1;
                write_now(true, rate, job.height, job.id, expected_hashes, eta,
                          last_error);
                last_status_write = now;
            }

            for (const auto &f : found) {
                solutions++;
                printf("\n*** solution: nonce %llu, hash %s\n",
                       (unsigned long long)f.nonce,
                       ab::hex_encode(f.hash, 32).c_str());
                if (o.dry_run) {
                    printf("dry-run: not submitting\n");
                    advance = true;
                    break;
                }
                // A found block must not be lost to a transient network
                // drop. Retry with backoff; the job lives for 2 minutes and
                // the server treats a resubmitted nonce as a duplicate, not
                // as a second block.
                bool accepted = false, stale = false, done = false;
                for (int attempt = 0; attempt < 6 && !g_stop; ++attempt) {
                    ab::HttpResponse sr;
                    std::string serr;
                    bool sent = submit(job, o.url, f.nonce, address, sr, serr);
                    if (sent) {
                        accepted = stale = false;
                        std::string block_hash, reason;
                        ab::json_bool(sr.body, "accepted", accepted);
                        ab::json_bool(sr.body, "stale", stale);
                        ab::json_string(sr.body, "hash", block_hash);
                        ab::json_string(sr.body, "reason", reason);
                        if (accepted) {
                            blocks++;
                            last_error.clear();
                            printf("*** BLOCK ACCEPTED at height %u\n", job.height);
                            if (!block_hash.empty())
                                printf("*** block hash: %s\n", block_hash.c_str());
                            advance = true;
                            done = true;
                            break;
                        }
                        if (stale) {
                            stale_count++;
                            printf("stale (another block won); fetching new work\n");
                            advance = true;
                            done = true;
                            break;
                        }
                        if (sr.status == 200) {
                            bool retryable =
                                reason.find("busy") != std::string::npos ||
                                reason.find("too many requests") != std::string::npos ||
                                reason.find("node unavailable") != std::string::npos;
                            if (retryable) {
                                last_error = reason;
                                printf("submit busy (%s); retrying %d/6\n",
                                       reason.c_str(), attempt + 1);
                            } else if (reason.find("duplicate") != std::string::npos) {
                                rejected_count++;
                                printf("submit says duplicate: %s\n", reason.c_str());
                                printf("(the first request may have reached the server; "
                                       "check the address page)\n");
                                advance = true;
                                done = true;
                                break;
                            } else {
                                rejected_count++;
                                last_error = reason.empty() ? "rejected" : reason;
                                printf("rejected: %s\n",
                                       reason.empty() ? "no reason" : reason.c_str());
                                advance = true;
                                done = true;
                                break;
                            }
                        } else {
                            last_error = "submit HTTP " + std::to_string(sr.status);
                            printf("submit HTTP %d (%s); retrying %d/6\n", sr.status,
                                   reason.empty() ? sr.body.c_str() : reason.c_str(),
                                   attempt + 1);
                        }
                    } else {
                        last_error = serr;
                        printf("submit failed: %s; retrying %d/6\n", serr.c_str(),
                               attempt + 1);
                    }
                    write_now(true, rate, job.height, job.id, expected_hashes, -1,
                              last_error);
                    int delay_ms = (attempt < 3) ? (1000 << attempt) : 8000;
                    for (int i = 0; i < delay_ms / 100 && !g_stop; ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (!done && !g_stop) {
                    rejected_count++;
                    printf("giving up on nonce %llu; fetching new work\n",
                           (unsigned long long)f.nonce);
                    advance = true;
                }
                if (advance) break;
            }
            if (advance) break;

            auto now2 = Clock::now();
            if (std::chrono::duration<double>(now2 - last_status).count() >= 2.0) {
                last_status = now2;
                double eta = (rate > 1 && expected_hashes > 0)
                                 ? expected_hashes / rate
                                 : -1;
                printf("\r[height %u] %s | total %s | blocks %llu | job %s | ETA %s   ",
                       job.height, fmt_rate(rate).c_str(),
                       fmt_count((double)total_hashes).c_str(),
                       (unsigned long long)blocks,
                       job.id.substr(0, 8).c_str(),
                       eta < 0 ? "?" : fmt_duration(eta).c_str());
                fflush(stdout);
            }
        }
        printf("\n");
    }

    printf("\nStopped. total hashes: %s, blocks accepted: %llu\n",
           fmt_count((double)total_hashes).c_str(),
           (unsigned long long)blocks);
    write_now(false, rate, 0, "", 0, -1, last_error);
    return 0;
}

int main(int argc, char **argv) {
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    setvbuf(stdout, nullptr, _IONBF, 0);
    // Keep Windows from sleeping while the miner is running. The display may
    // still turn off; this only prevents system sleep.
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    bool interactive = (argc == 1);
    int rc = run_miner(argc, argv, interactive);
    SetThreadExecutionState(ES_CONTINUOUS);
    if (interactive) {
        printf("\nPress Enter to close...");
        std::string dummy;
        std::getline(std::cin, dummy);
    }
    return rc;
}
