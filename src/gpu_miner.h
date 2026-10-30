// GPU Argon2id (t=1, m=8192 KiB, p=1, 32-byte tag) for Argonmine.
//
// Design:
//   * init_kernel   : one thread per nonce computes H0 and expands B[0], B[1].
//   * fill_kernel   : one warp handles 4 hashes; each 8-lane group handles one
//                     hash; each lane owns one 16-word (128-byte) row/column
//                     slice and keeps it in registers across the two phases.
//   * final_kernel  : one thread per nonce computes the tag and compares it
//                     with the block target.
//
// The memory-hard fill is the hot path and uses 3 KiB of global traffic per
// 1 KiB block (prev + ref read, result write), plus shared-memory staging.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace ab {

struct GpuFound {
    uint64_t nonce = 0;
    uint32_t slot = 0;
    uint8_t hash[32] = {};
};

struct GpuInfo {
    std::string name;
    int major = 0;
    int minor = 0;
    size_t total_mem = 0;
    int sm_count = 0;
    int max_threads_per_sm = 0;
};

class GpuMiner {
public:
    GpuMiner();
    ~GpuMiner();

    // device < 0 picks the first CUDA device. batch is rounded up to a multiple
    // of 16 and reduced if the allocation does not fit.
    bool init(int device, int batch, std::string &err);
    GpuInfo info() const { return info_; }
    int batch() const { return batch_; }
    size_t memory_bytes() const { return mem_bytes_; }

    // Set the current block template (112-byte header prefix + target).
    bool set_job(const uint8_t prefix[112], const uint8_t target_be[32],
                 std::string &err);

    // One batch of nonces [base, base+batch). Any solutions are appended.
    bool mine(uint64_t base, std::vector<GpuFound> &found, double *seconds,
              std::string &err);

    // Debug/self-test helper: hash one header (112-byte prefix + nonce) and
    // return the 32-byte Argon2id tag. Uses slot 0 of a 16-wide batch.
    bool hash_one(const uint8_t prefix[112], uint64_t nonce, uint8_t tag[32],
                  std::string &err);

private:
    bool run_batch(const uint8_t prefix[112], uint64_t target_limbs[4],
                   int count, uint64_t base, std::vector<GpuFound> &found,
                   double *seconds, std::string &err);

    GpuInfo info_;
    int batch_ = 0;
    size_t mem_bytes_ = 0;
    uint8_t prefix_[112] = {};
    uint64_t target_[4] = {};
    uint8_t *d_mem_ = nullptr;
    uint64_t *d_addr_ = nullptr;
    uint32_t *d_found_count_ = nullptr;
    GpuFound *d_found_ = nullptr;
    void *stream_ = nullptr;
    void *ev_start_ = nullptr;
    void *ev_end_ = nullptr;
};

// Argon2 address-table constants for (pass=0, lane=0, m=8192, t=1, type=2).
// 4 slices x 16 address blocks x 128 words.
void argon2_address_table(uint64_t out[4][16][128]);

}  // namespace ab
