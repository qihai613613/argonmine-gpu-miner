#include "gpu_miner.h"

#include <cuda_runtime.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <sstream>

namespace ab {
namespace {

constexpr int kWords = 128;
constexpr int kBlockBytes = 1024;
constexpr int kBlocks = 8192;
constexpr int kSeg = 2048;
constexpr int kSlices = 4;
constexpr int kHPW = 4;            // hashes per warp
constexpr int kWarpsPerBlock = 4;  // 128 threads
constexpr int kHashesPerBlock = kHPW * kWarpsPerBlock;
constexpr int kMaxFound = 64;

struct Prefix112 {
    uint8_t b[112];
};

struct Target4 {
    uint64_t w[4];
};

struct H0In {
    uint32_t lanes;
    uint32_t outlen;
    uint32_t memory;
    uint32_t passes;
    uint32_t version;
    uint32_t type;
    uint32_t pwdlen;
    uint8_t pwd[120];
    uint32_t saltlen;
    uint8_t salt[16];
    uint32_t keylen;
    uint32_t adlen;
};
static_assert(sizeof(H0In) == 176, "H0 input layout");

// ---------------------------------------------------------------------------
// Blake2b (device, one-shot)
// ---------------------------------------------------------------------------

__device__ __constant__ uint8_t kSigma[12][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
    {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
    {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
    {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3}};

__device__ __forceinline__ uint64_t rotr64(uint64_t x, int n) {
    return (x >> n) | (x << (64 - n));
}

__device__ __forceinline__ void b2b_g(uint64_t &a, uint64_t &b, uint64_t &c,
                                      uint64_t &d, uint64_t x, uint64_t y) {
    a = a + b + x;
    d = rotr64(d ^ a, 32);
    c = c + d;
    b = rotr64(b ^ c, 24);
    a = a + b + y;
    d = rotr64(d ^ a, 16);
    c = c + d;
    b = rotr64(b ^ c, 63);
}

__device__ void b2b_compress(uint64_t h[8], const uint8_t *block, uint64_t t,
                             bool last) {
    uint64_t m[16];
#pragma unroll
    for (int i = 0; i < 16; ++i) {
        uint64_t x = 0;
#pragma unroll
        for (int j = 0; j < 8; ++j) x |= (uint64_t)block[i * 8 + j] << (8 * j);
        m[i] = x;
    }
    uint64_t v[16];
#pragma unroll
    for (int i = 0; i < 8; ++i) v[i] = h[i];
    v[8] = 0x6a09e667f3bcc908ULL;
    v[9] = 0xbb67ae8584caa73bULL;
    v[10] = 0x3c6ef372fe94f82bULL;
    v[11] = 0xa54ff53a5f1d36f1ULL;
    v[12] = 0x510e527fade682d1ULL ^ t;
    v[13] = 0x9b05688c2b3e6c1fULL;
    v[14] = 0x1f83d9abfb41bd6bULL;
    v[15] = 0x5be0cd19137e2179ULL;
    if (last) v[14] = ~v[14];

#pragma unroll
    for (int r = 0; r < 12; ++r) {
        const uint8_t *s = kSigma[r];
        b2b_g(v[0], v[4], v[8], v[12], m[s[0]], m[s[1]]);
        b2b_g(v[1], v[5], v[9], v[13], m[s[2]], m[s[3]]);
        b2b_g(v[2], v[6], v[10], v[14], m[s[4]], m[s[5]]);
        b2b_g(v[3], v[7], v[11], v[15], m[s[6]], m[s[7]]);
        b2b_g(v[0], v[5], v[10], v[15], m[s[8]], m[s[9]]);
        b2b_g(v[1], v[6], v[11], v[12], m[s[10]], m[s[11]]);
        b2b_g(v[2], v[7], v[8], v[13], m[s[12]], m[s[13]]);
        b2b_g(v[3], v[4], v[9], v[14], m[s[14]], m[s[15]]);
    }
#pragma unroll
    for (int i = 0; i < 8; ++i) h[i] ^= v[i] ^ v[i + 8];
}

__device__ void blake2b_hash(uint8_t *out, size_t outlen, const uint8_t *in,
                             size_t inlen) {
    uint64_t h[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
                     0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                     0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
                     0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
    h[0] ^= 0x01010000ULL ^ (uint64_t)outlen;

    size_t off = 0;
    uint64_t t = 0;
    while (inlen - off > 128) {
        t += 128;
        b2b_compress(h, in + off, t, false);
        off += 128;
    }
    uint8_t buf[128];
#pragma unroll
    for (int i = 0; i < 128; ++i) buf[i] = 0;
    size_t rem = inlen - off;
    for (size_t i = 0; i < rem; ++i) buf[i] = in[off + i];
    t += rem;
    b2b_compress(h, buf, t, true);

    for (size_t i = 0; i < outlen; ++i)
        out[i] = (uint8_t)(h[i >> 3] >> (8 * (i & 7)));
}

// H' for a 1024-byte Argon2 block from a 72-byte seed.
__device__ void blake2b_long_1024(uint8_t *out, const uint8_t *seed72) {
    uint8_t in[76];
    in[0] = 0;
    in[1] = 4;  // 1024 little-endian
    in[2] = 0;
    in[3] = 0;
#pragma unroll
    for (int i = 0; i < 72; ++i) in[4 + i] = seed72[i];

    uint8_t h[64];
    blake2b_hash(h, 64, in, 76);
#pragma unroll
    for (int i = 0; i < 32; ++i) out[i] = h[i];
    out += 32;

    for (int i = 0; i < 29; ++i) {
        uint8_t tmp[64];
        blake2b_hash(tmp, 64, h, 64);
#pragma unroll
        for (int j = 0; j < 64; ++j) h[j] = tmp[j];
#pragma unroll
        for (int j = 0; j < 32; ++j) out[j] = h[j];
        out += 32;
    }
    uint8_t tmp[64];
    blake2b_hash(tmp, 64, h, 64);
#pragma unroll
    for (int j = 0; j < 64; ++j) out[j] = tmp[j];
}

// ---------------------------------------------------------------------------
// Argon2 fill primitives
// ---------------------------------------------------------------------------

__device__ __forceinline__ void blamka_g(uint64_t &a, uint64_t &b, uint64_t &c,
                                         uint64_t &d) {
    a += b + 2ULL * (uint64_t)(uint32_t)a * (uint64_t)(uint32_t)b;
    d = rotr64(d ^ a, 32);
    c += d + 2ULL * (uint64_t)(uint32_t)c * (uint64_t)(uint32_t)d;
    b = rotr64(b ^ c, 24);
    a += b + 2ULL * (uint64_t)(uint32_t)a * (uint64_t)(uint32_t)b;
    d = rotr64(d ^ a, 16);
    c += d + 2ULL * (uint64_t)(uint32_t)c * (uint64_t)(uint32_t)d;
    b = rotr64(b ^ c, 63);
}

__device__ __forceinline__ void round16(uint64_t *v) {
    blamka_g(v[0], v[4], v[8], v[12]);
    blamka_g(v[1], v[5], v[9], v[13]);
    blamka_g(v[2], v[6], v[10], v[14]);
    blamka_g(v[3], v[7], v[11], v[15]);
    blamka_g(v[0], v[5], v[10], v[15]);
    blamka_g(v[1], v[6], v[11], v[12]);
    blamka_g(v[2], v[7], v[8], v[13]);
    blamka_g(v[3], v[4], v[9], v[14]);
}

// ---------------------------------------------------------------------------
// Kernels
// ---------------------------------------------------------------------------

__global__ void init_kernel(uint8_t *mem, Prefix112 prefix, uint64_t base_nonce,
                            int count) {
    int slot = blockIdx.x * blockDim.x + threadIdx.x;
    if (slot >= count) return;
    uint64_t nonce = base_nonce + (uint64_t)slot;

    H0In in;
    in.lanes = 1;
    in.outlen = 32;
    in.memory = 8192;
    in.passes = 1;
    in.version = 0x13;
    in.type = 2;
    in.pwdlen = 120;
#pragma unroll
    for (int i = 0; i < 112; ++i) in.pwd[i] = prefix.b[i];
#pragma unroll
    for (int i = 0; i < 8; ++i) in.pwd[112 + i] = (uint8_t)(nonce >> (8 * i));
    in.saltlen = 16;
    const char *salt = "Argonmine/pow/v1";
#pragma unroll
    for (int i = 0; i < 16; ++i) in.salt[i] = (uint8_t)salt[i];
    in.keylen = 0;
    in.adlen = 0;

    uint8_t h0[64];
    blake2b_hash(h0, 64, reinterpret_cast<const uint8_t *>(&in), sizeof(in));

    uint8_t *dst = mem + (size_t)slot * (kBlocks * kBlockBytes);
    uint8_t seed[72];
#pragma unroll
    for (int i = 0; i < 64; ++i) seed[i] = h0[i];
    seed[64] = 0;
    seed[65] = 0;
    seed[66] = 0;
    seed[67] = 0;
    seed[68] = 0;
    seed[69] = 0;
    seed[70] = 0;
    seed[71] = 0;
    blake2b_long_1024(dst, seed);
    seed[64] = 1;
    blake2b_long_1024(dst + kBlockBytes, seed);
}

__global__ void __launch_bounds__(128, 4)
    fill_kernel(uint8_t *mem, const uint64_t *__restrict__ addr, int count) {
    extern __shared__ uint64_t smem[];
    const int lane = threadIdx.x & 31;
    const int warp = threadIdx.x >> 5;
    const int group = lane >> 3;  // hash within the warp
    const int ll = lane & 7;      // row/column lane within the hash

    const int slot = (blockIdx.x * kWarpsPerBlock + warp) * kHPW + group;
    // count is always a multiple of kHashesPerBlock, so the whole warp is valid
    // and every __syncwarp() below is reached by all 32 lanes.
    if (slot >= count) return;

    uint64_t *blk = smem + (warp * kHPW + group) * kWords;
    uint8_t *slotmem = mem + (size_t)slot * (kBlocks * kBlockBytes);
    uint64_t prev_first =
        *reinterpret_cast<const uint64_t *>(slotmem + kBlockBytes);

#pragma unroll 1
    for (int slice = 0; slice < kSlices; ++slice) {
        const int start = (slice == 0) ? 2 : 0;
#pragma unroll 1
        for (int index = start; index < kSeg; ++index) {
            const int offset = slice * kSeg + index;
            uint64_t random;
            if (slice < 2) {
                const int ctr = index >> 7;  // 0..15
                random = addr[(slice * 16 + ctr) * 128 + (index & 127)];
            } else {
                random = prev_first;
            }

            uint32_t r32 = (uint32_t)random;
            uint64_t p = (uint64_t)r32 * (uint64_t)r32;
            p >>= 32;
            const uint64_t m = (uint64_t)(slice * kSeg + index - 1);
            p = (p * m) >> 32;
            const uint32_t ref = (uint32_t)(m - 1 - p);

            const uint8_t *prevp = slotmem + (size_t)(offset - 1) * kBlockBytes;
            const uint8_t *refp = slotmem + (size_t)ref * kBlockBytes;

            uint64_t R[16];
            {
                const ulonglong2 *pp =
                    reinterpret_cast<const ulonglong2 *>(prevp + ll * 128);
                const ulonglong2 *rp =
                    reinterpret_cast<const ulonglong2 *>(refp + ll * 128);
#pragma unroll
                for (int k = 0; k < 8; ++k) {
                    ulonglong2 a = pp[k];
                    ulonglong2 b = rp[k];
                    R[2 * k] = a.x ^ b.x;
                    R[2 * k + 1] = a.y ^ b.y;
                }
            }

            uint64_t V[16];
#pragma unroll
            for (int k = 0; k < 16; ++k) V[k] = R[k];
            round16(V);
#pragma unroll
            for (int k = 0; k < 16; ++k) blk[ll * 16 + k] = V[k];
            __syncwarp();

#pragma unroll
            for (int k = 0; k < 8; ++k) {
                V[2 * k] = blk[2 * ll + 16 * k];
                V[2 * k + 1] = blk[2 * ll + 1 + 16 * k];
            }
            round16(V);
#pragma unroll
            for (int k = 0; k < 8; ++k) {
                blk[2 * ll + 16 * k] = V[2 * k];
                blk[2 * ll + 1 + 16 * k] = V[2 * k + 1];
            }
            __syncwarp();

            uint8_t *outp = slotmem + (size_t)offset * kBlockBytes;
            ulonglong2 *op = reinterpret_cast<ulonglong2 *>(outp + ll * 128);
#pragma unroll
            for (int k = 0; k < 8; ++k) {
                uint64_t a = blk[ll * 16 + 2 * k] ^ R[2 * k];
                uint64_t b = blk[ll * 16 + 2 * k + 1] ^ R[2 * k + 1];
                op[k] = make_ulonglong2(a, b);
            }
            uint64_t w0 = (ll == 0) ? (blk[0] ^ R[0]) : 0;
            w0 = __shfl_sync(0xffffffffu, w0, group * 8);
            prev_first = w0;
            __syncwarp();
        }
    }
}

__global__ void final_kernel(const uint8_t *__restrict__ mem,
                             GpuFound *__restrict__ found,
                             uint32_t *found_count, Target4 target,
                             uint64_t base_nonce, int count) {
    int slot = blockIdx.x * blockDim.x + threadIdx.x;
    if (slot >= count) return;

    const uint8_t *last =
        mem + (size_t)slot * (kBlocks * kBlockBytes) +
        (size_t)(kBlocks - 1) * kBlockBytes;

    uint8_t in[1028];
    in[0] = 32;
    in[1] = 0;
    in[2] = 0;
    in[3] = 0;
#pragma unroll 4
    for (int i = 0; i < 1024; ++i) in[4 + i] = last[i];
    uint8_t tag[32];
    blake2b_hash(tag, 32, in, sizeof(in));

    uint64_t w[4];
#pragma unroll
    for (int i = 0; i < 4; ++i) {
        uint64_t x = 0;
#pragma unroll
        for (int j = 0; j < 8; ++j) x |= (uint64_t)tag[i * 8 + j] << (8 * j);
        w[i] = x;
    }

    bool ok;
    if (w[3] != target.w[3])
        ok = w[3] < target.w[3];
    else if (w[2] != target.w[2])
        ok = w[2] < target.w[2];
    else if (w[1] != target.w[1])
        ok = w[1] < target.w[1];
    else
        ok = w[0] <= target.w[0];

    if (ok) {
        uint32_t idx = atomicAdd(found_count, 1u);
        if (idx < kMaxFound) {
            found[idx].nonce = base_nonce + (uint64_t)slot;
            found[idx].slot = (uint32_t)slot;
#pragma unroll
            for (int i = 0; i < 32; ++i) found[idx].hash[i] = tag[i];
        }
    }
}

// ---------------------------------------------------------------------------
// Host helpers
// ---------------------------------------------------------------------------

std::string cuda_err(const char *what, cudaError_t e) {
    std::ostringstream os;
    os << what << ": " << cudaGetErrorString(e);
    return os.str();
}

uint64_t be64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

}  // namespace

GpuMiner::GpuMiner() = default;

GpuMiner::~GpuMiner() {
    if (stream_) cudaStreamDestroy(reinterpret_cast<cudaStream_t>(stream_));
    if (ev_start_) cudaEventDestroy(reinterpret_cast<cudaEvent_t>(ev_start_));
    if (ev_end_) cudaEventDestroy(reinterpret_cast<cudaEvent_t>(ev_end_));
    if (d_mem_) cudaFree(d_mem_);
    if (d_addr_) cudaFree(d_addr_);
    if (d_found_) cudaFree(d_found_);
    if (d_found_count_) cudaFree(d_found_count_);
}

bool GpuMiner::init(int device, int batch, std::string &err) {
    int ndev = 0;
    cudaError_t e = cudaGetDeviceCount(&ndev);
    if (e != cudaSuccess || ndev == 0) {
        err = "no CUDA device: " + std::string(cudaGetErrorString(e));
        return false;
    }
    if (device < 0) device = 0;
    if (device >= ndev) {
        err = "CUDA device index out of range";
        return false;
    }
    e = cudaSetDevice(device);
    if (e != cudaSuccess) {
        err = cuda_err("cudaSetDevice", e);
        return false;
    }
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, device);
    info_.name = prop.name;
    info_.major = prop.major;
    info_.minor = prop.minor;
    info_.total_mem = prop.totalGlobalMem;
    info_.sm_count = prop.multiProcessorCount;
    info_.max_threads_per_sm = prop.maxThreadsPerMultiProcessor;

    if (batch < kHashesPerBlock) batch = kHashesPerBlock;
    batch = (batch / kHashesPerBlock) * kHashesPerBlock;

    // Keep at least ~1.5 GiB of the card free for the display/driver.
    size_t budget = prop.totalGlobalMem;
    if (budget > (size_t)2 << 30) budget -= (size_t)3 << 29;
    size_t per_hash = (size_t)kBlocks * kBlockBytes;
    int max_by_mem = (int)(budget / per_hash);
    max_by_mem = (max_by_mem / kHashesPerBlock) * kHashesPerBlock;
    if (max_by_mem < kHashesPerBlock) max_by_mem = kHashesPerBlock;
    if (batch > max_by_mem) batch = max_by_mem;

    while (batch >= kHashesPerBlock) {
        e = cudaMalloc(&d_mem_, (size_t)batch * per_hash);
        if (e == cudaSuccess) break;
        cudaGetLastError();
        batch -= kHashesPerBlock;
    }
    if (!d_mem_) {
        err = "cudaMalloc failed for the Argon2 memory buffer";
        return false;
    }
    batch_ = batch;
    mem_bytes_ = (size_t)batch_ * per_hash;

    e = cudaMalloc(&d_addr_, 4 * 16 * 128 * sizeof(uint64_t));
    if (e != cudaSuccess) {
        err = cuda_err("cudaMalloc(addr)", e);
        return false;
    }
    static uint64_t addr_table[4][16][128];
    argon2_address_table(addr_table);
    e = cudaMemcpy(d_addr_, addr_table, sizeof(addr_table),
                   cudaMemcpyHostToDevice);
    if (e != cudaSuccess) {
        err = cuda_err("cudaMemcpy(addr)", e);
        return false;
    }

    e = cudaMalloc(&d_found_, kMaxFound * sizeof(GpuFound));
    if (e != cudaSuccess) {
        err = cuda_err("cudaMalloc(found)", e);
        return false;
    }
    e = cudaMalloc(&d_found_count_, sizeof(uint32_t));
    if (e != cudaSuccess) {
        err = cuda_err("cudaMalloc(count)", e);
        return false;
    }

    cudaStream_t s;
    e = cudaStreamCreate(&s);
    if (e != cudaSuccess) {
        err = cuda_err("cudaStreamCreate", e);
        return false;
    }
    stream_ = s;
    cudaEvent_t a, b;
    cudaEventCreate(&a);
    cudaEventCreate(&b);
    ev_start_ = a;
    ev_end_ = b;

    cudaFuncSetAttribute(fill_kernel,
                         cudaFuncAttributePreferredSharedMemoryCarveout,
                         cudaSharedmemCarveoutMaxShared);
    return true;
}

bool GpuMiner::set_job(const uint8_t prefix[112], const uint8_t target_be[32],
                       std::string &) {
    std::memcpy(prefix_, prefix, 112);
    // Target is a big-endian 256-bit number; the four limbs are little-
    // endian-order indices T0 (least significant) .. T3 (most significant).
    target_[0] = be64(target_be + 24);
    target_[1] = be64(target_be + 16);
    target_[2] = be64(target_be + 8);
    target_[3] = be64(target_be + 0);
    return true;
}

bool GpuMiner::run_batch(const uint8_t prefix[112], uint64_t target_limbs[4],
                         int count, uint64_t base, std::vector<GpuFound> &found,
                         double *seconds, std::string &err) {
    if (count <= 0) return true;
    if (count % kHashesPerBlock != 0) {
        err = "internal: batch not a multiple of 16";
        return false;
    }
    cudaStream_t s = reinterpret_cast<cudaStream_t>(stream_);
    cudaEvent_t ev0 = reinterpret_cast<cudaEvent_t>(ev_start_);
    cudaEvent_t ev1 = reinterpret_cast<cudaEvent_t>(ev_end_);

    cudaError_t e = cudaMemsetAsync(d_found_count_, 0, sizeof(uint32_t), s);
    if (e != cudaSuccess) {
        err = cuda_err("cudaMemsetAsync", e);
        return false;
    }

    Prefix112 p;
    std::memcpy(p.b, prefix, 112);
    Target4 tg;
    for (int i = 0; i < 4; ++i) tg.w[i] = target_limbs[i];

    cudaEventRecord(ev0, s);
    int init_blocks = (count + 127) / 128;
    init_kernel<<<init_blocks, 128, 0, s>>>(d_mem_, p, base, count);

    int fill_blocks = count / kHashesPerBlock;
    size_t shmem = (size_t)kWarpsPerBlock * kHPW * kWords * sizeof(uint64_t);
    fill_kernel<<<fill_blocks, 128, shmem, s>>>(d_mem_, d_addr_, count);

    int final_blocks = (count + 127) / 128;
    final_kernel<<<final_blocks, 128, 0, s>>>(d_mem_, d_found_, d_found_count_,
                                              tg, base, count);
    cudaEventRecord(ev1, s);
    e = cudaStreamSynchronize(s);
    if (e != cudaSuccess) {
        err = cuda_err("kernel", e);
        return false;
    }
    if (seconds) {
        float ms = 0;
        cudaEventElapsedTime(&ms, ev0, ev1);
        *seconds = ms / 1000.0;
    }

    uint32_t n = 0;
    e = cudaMemcpy(&n, d_found_count_, sizeof(n), cudaMemcpyDeviceToHost);
    if (e != cudaSuccess) {
        err = cuda_err("cudaMemcpy(count)", e);
        return false;
    }
    n = std::min<uint32_t>(n, kMaxFound);
    if (n) {
        std::vector<GpuFound> tmp(n);
        e = cudaMemcpy(tmp.data(), d_found_, n * sizeof(GpuFound),
                       cudaMemcpyDeviceToHost);
        if (e != cudaSuccess) {
            err = cuda_err("cudaMemcpy(found)", e);
            return false;
        }
        found.insert(found.end(), tmp.begin(), tmp.end());
    }
    return true;
}

bool GpuMiner::mine(uint64_t base, std::vector<GpuFound> &found,
                    double *seconds, std::string &err) {
    return run_batch(prefix_, target_, batch_, base, found, seconds, err);
}

bool GpuMiner::hash_one(const uint8_t prefix[112], uint64_t nonce,
                        uint8_t tag[32], std::string &err) {
    uint64_t max_target[4] = {~0ULL, ~0ULL, ~0ULL, ~0ULL};
    std::vector<GpuFound> found;
    if (!run_batch(prefix, max_target, kHashesPerBlock, nonce, found, nullptr,
                   err))
        return false;
    for (const auto &f : found) {
        if (f.slot == 0) {
            std::memcpy(tag, f.hash, 32);
            return true;
        }
    }
    err = "hash_one: no result";
    return false;
}

}  // namespace ab
