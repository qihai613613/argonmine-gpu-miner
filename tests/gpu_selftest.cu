#include "gpu_miner.h"
#include "argon2_cpu.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>

static uint64_t rng_state = 0x123456789abcdef0ULL;
static uint64_t xrand() {
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

static std::string hex(const uint8_t *p, size_t n) {
    static const char *d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 15]);
    }
    return s;
}

int main(int argc, char **argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 4;
    int req_batch = (argc > 2) ? atoi(argv[2]) : 1024;
    ab::GpuMiner gpu;
    std::string err;
    if (!gpu.init(-1, req_batch, err)) {
        printf("GPU init failed: %s\n", err.c_str());
        return 1;
    }
    auto inf = gpu.info();
    printf("GPU: %s  sm_%d%d  %d SMs  %.1f GiB  batch=%d\n", inf.name.c_str(),
           inf.major, inf.minor, inf.sm_count,
           inf.total_mem / 1073741824.0, gpu.batch());

    bool all = true;
    for (int i = 0; i < n; ++i) {
        uint8_t hdr[120];
        for (int j = 0; j < 120; j += 8) {
            uint64_t v = xrand();
            memcpy(hdr + j, &v, 8);
        }
        uint8_t prefix[112];
        memcpy(prefix, hdr, 112);
        uint64_t nonce;
        memcpy(&nonce, hdr + 112, 8);

        ab::Argon2Params p;
        auto cpu = ab::argon2id(hdr, sizeof(hdr),
                                (const uint8_t *)"Argonmine/pow/v1", 16, p);
        uint8_t gpu_tag[32];
        auto t0 = std::chrono::steady_clock::now();
        if (!gpu.hash_one(prefix, nonce, gpu_tag, err)) {
            printf("hash_one failed: %s\n", err.c_str());
            return 1;
        }
        auto t1 = std::chrono::steady_clock::now();
        bool ok = memcmp(cpu.data(), gpu_tag, 32) == 0;
        all = all && ok;
        printf("[%d] %s  %.1f ms  cpu=%s gpu=%s\n", i, ok ? "OK" : "MISMATCH",
               std::chrono::duration<double, std::milli>(t1 - t0).count(),
               hex(cpu.data(), 32).c_str(), hex(gpu_tag, 32).c_str());
    }

    if (all) {
        uint8_t z[112] = {};
        uint8_t target[32];
        memset(target, 0, 32);
        gpu.set_job(z, target, err);
        std::vector<ab::GpuFound> found;
        double sec = 0;
        int batch = gpu.batch();
        // warm-up
        gpu.mine(0, found, &sec, err);
        found.clear();
        double best = 1e9, total = 0;
        for (int it = 0; it < 5; ++it) {
            uint64_t base = 0x100000000ULL + (uint64_t)it * batch;
            gpu.mine(base, found, &sec, err);
            if (sec < best) best = sec;
            total += sec;
            printf("  run %d: %.3f s = %.1f H/s\n", it, sec, batch / sec);
        }
        printf("bench: %d hashes, best %.1f H/s, avg %.1f H/s (found=%zu)\n",
               batch, batch / best, 5.0 * batch / total, found.size());
    }
    printf(all ? "ALL OK\n" : "FAILED\n");
    return all ? 0 : 2;
}
