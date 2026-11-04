// hpd_gpu.cuh — the model loop on an NVIDIA GPU (hpd-gpu, built with clang -x cuda -DHPD_GPU).
//
// One GPU thread codes one shard (block) with its own hp::Predictor, exactly as
// the CPU does, so a GPU archive is byte-identical to the CPU archive with the
// same settings and either build decodes it. The CPU still does everything
// around the model: CRC, x86 transform, pass-through chunks, the container.
//
// Memory: each shard gets one slice of GPU memory (its arena) holding the
// Predictor and every table. The slice size is measured once per model setting
// by a probe kernel. Work runs as many short launches (~100 ms each) so the
// Windows display driver never kills a long kernel (WDDM TDR, also under WSL2).
// Included by hpd.cpp inside its anonymous namespace.

#include <cuda_runtime.h>

namespace gpu {

struct Job {                       // lives in device memory
    const std::uint8_t* in;        // encode: bytes to code; decode: CM payload
    std::uint64_t in_len;
    std::uint8_t* out;             // encode: payload buffer; decode: decoded bytes
    std::uint64_t out_cap;
    std::uint64_t n;               // bytes to code (encode) / to produce (decode)
    int table_bits, slot_extra, disc_slots;
    hp::Predictor* pred;
    std::uint64_t pos;             // bytes coded so far
    std::uint64_t io;              // encode: payload bytes written; decode: payload bytes read
    std::uint32_t x1, x2, x;
    int status;                    // 0 running, 1 done, 2 gave up (encode: the block will be stored)
};

__device__ inline std::uint32_t d_split(std::uint32_t x1, std::uint32_t x2, int p) {
    const std::uint32_t range = x2 - x1;
    return x1 + (range >> 16) * static_cast<std::uint32_t>(p) +
           (((range & 0xffffu) * static_cast<std::uint32_t>(p)) >> 16);
}

__global__ void k_init(Job* jobs, int nj, int decode) {
    const int t = blockIdx.x * blockDim.x + threadIdx.x;
    if (t >= nj) return;
    Job& j = jobs[t];
    hp::Config cfg;
    cfg.table_bits = j.table_bits;
    cfg.normalize();
    cfg.slot_extra = j.slot_extra;
    cfg.disc_slots = j.disc_slots;
    j.pred = new (hp::hp_dev_alloc(sizeof(hp::Predictor))) hp::Predictor(cfg);
    j.pos = 0; j.io = 0; j.x1 = 0; j.x2 = 0xffffffffu; j.x = 0;
    j.status = j.n ? 0 : 1;
    if (decode)
        for (int i = 0; i < 4; ++i) j.x = (j.x << 8) | (j.io < j.in_len ? j.in[j.io++] : 0u);
}

// Same loop, same give-up rule as compress_block. Output past out_cap means the
// payload is not smaller than the input, so the block is stored either way.
__global__ void k_encode(Job* jobs, int nj, std::uint64_t bytes) {
    const int t = blockIdx.x * blockDim.x + threadIdx.x;
    if (t >= nj) return;
    Job& j = jobs[t];
    if (j.status) return;
    hp::Predictor& p = *j.pred;
    std::uint32_t x1 = j.x1, x2 = j.x2;
    std::uint64_t k = j.pos, w = j.io;
    const std::uint64_t end = j.n < k + bytes ? j.n : k + bytes;
    int st = 0;
    for (; k < end && !st; ++k) {
        const int c = j.in[k];
        for (int i = 7; i >= 0; --i) {
            const int pr = p.predict();
            const int bit = (c >> i) & 1;
            const std::uint32_t xmid = d_split(x1, x2, hp::clamp_int(pr << 4, 1, 65535));
            if (bit) x2 = xmid; else x1 = xmid + 1;
            while (((x1 ^ x2) & 0xff000000u) == 0) {
                if (w < j.out_cap) j.out[w] = static_cast<std::uint8_t>(x2 >> 24);
                ++w;
                x1 <<= 8; x2 = (x2 << 8) | 255u;
            }
            p.update(bit);
        }
        if (w > j.out_cap) st = 2;
        if ((k & 0xffff) == 0xffff && k >= (1u << 20) && w > k) st = 2;
    }
    if (!st && k == j.n) {
        for (int i = 0; i < 4; ++i) {
            if (w < j.out_cap) j.out[w] = static_cast<std::uint8_t>(x1 >> 24);
            ++w;
            x1 <<= 8;
        }
        st = w > j.out_cap ? 2 : 1;
    }
    j.x1 = x1; j.x2 = x2; j.pos = k; j.io = w; j.status = st;
}

__global__ void k_decode(Job* jobs, int nj, std::uint64_t bytes) {
    const int t = blockIdx.x * blockDim.x + threadIdx.x;
    if (t >= nj) return;
    Job& j = jobs[t];
    if (j.status) return;
    hp::Predictor& p = *j.pred;
    std::uint32_t x1 = j.x1, x2 = j.x2, x = j.x;
    std::uint64_t k = j.pos, r = j.io;
    const std::uint64_t end = j.n < k + bytes ? j.n : k + bytes;
    for (; k < end; ++k) {
        int byte = 0;
        for (int i = 0; i < 8; ++i) {
            const std::uint32_t xmid = d_split(x1, x2, hp::clamp_int(p.predict() << 4, 1, 65535));
            const int bit = x <= xmid ? 1 : 0;
            if (bit) x2 = xmid; else x1 = xmid + 1;
            while (((x1 ^ x2) & 0xff000000u) == 0) {
                x1 <<= 8; x2 = (x2 << 8) | 255u;
                x = (x << 8) | (r < j.in_len ? j.in[r++] : 0u);
            }
            p.update(bit);
            byte = (byte << 1) | bit;
        }
        j.out[k] = static_cast<std::uint8_t>(byte);
    }
    j.x1 = x1; j.x2 = x2; j.x = x; j.pos = k; j.io = r;
    if (k == j.n) j.status = 1;
}

// Probe: build one Predictor in a large arena, code 4 KiB, report bytes used.
__global__ void k_probe(int table_bits, int slot_extra, int disc_slots, std::uint64_t* used) {
    hp::Config cfg;
    cfg.table_bits = table_bits;
    cfg.normalize();
    cfg.slot_extra = slot_extra;
    cfg.disc_slots = disc_slots;
    hp::Predictor* p = new (hp::hp_dev_alloc(sizeof(hp::Predictor))) hp::Predictor(cfg);
    std::uint32_t s = 12345;
    for (int k = 0; k < 4096; ++k) {
        s = s * 1103515245u + 12345u;
        const int c = (k & 1) ? static_cast<int>(s >> 24) : "probe text "[k % 11];
        for (int i = 7; i >= 0; --i) { (void)p->predict(); p->update((c >> i) & 1); }
    }
    *used = hp::hp_my_arena().used;
}

// ------------------------------------------------------------------ host side

struct Task {                      // one shard for the GPU
    const std::uint8_t* in;
    std::uint64_t in_len;
    std::uint64_t n;               // encode: bytes to code; decode: bytes to produce
    std::uint64_t out_cap;
    int table_bits, slot_extra, disc_slots;
    std::vector<std::uint8_t>* out;   // result (resized to the bytes produced)
    bool gave_up = false;
};

struct Device {
    bool ready = false;
    std::string error;
    std::string name;
    std::size_t total = 0;
    double ms_per_byte = 0;       // measured per launch, all shards in parallel
};
Device g_dev;

bool trace() {
    static const bool on = std::getenv("HPD_GPU_TRACE") != nullptr;
    return on;
}
double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

bool check(cudaError_t e, const char* what) {
    if (e == cudaSuccess) return true;
    if (g_dev.error.empty()) g_dev.error = std::string(what) + ": " + cudaGetErrorString(e);
    return false;
}

bool init() {
    if (g_dev.ready) return true;
    if (!g_dev.error.empty()) return false;
    int n = 0;
    if (!check(cudaGetDeviceCount(&n), "no CUDA device") || n == 0) {
        if (g_dev.error.empty()) g_dev.error = "no CUDA device";
        return false;
    }
    cudaDeviceProp prop{};
    if (!check(cudaGetDeviceProperties(&prop, 0), "cudaGetDeviceProperties")) return false;
    g_dev.name = prop.name;
    g_dev.total = prop.totalGlobalMem;
    if (!check(cudaDeviceSetLimit(cudaLimitStackSize, 16384), "stack size")) return false;
    // Tables the model builds at start-up on the CPU, copied once to the GPU.
    if (!check(cudaMemcpyToSymbol(hp::hp_dev_stretch, &hp::stretch_table(), sizeof(hp::StretchTable)), "upload tables")) return false;
    if (!check(cudaMemcpyToSymbol(hp::hp_dev_log2, &hp::log2_table(), sizeof(hp::Log2Table)), "upload tables")) return false;
    if (!check(cudaMemcpyToSymbol(hp::hp_dev_state, &hp::state_table(), sizeof(hp::StateTable)), "upload tables")) return false;
    if (!check(cudaMemcpyToSymbol(hp::hp_dev_py, &hp::py_tables(), sizeof(hp::PyTables)), "upload tables")) return false;
    g_dev.ready = true;
    return true;
}

// Arena bytes for one shard with these settings (measured once, cached).
std::uint64_t arena_bytes(int table_bits, int slot_extra, int disc_slots) {
    static std::vector<std::array<std::uint64_t, 4>> cache;
    for (const auto& c : cache)
        if (c[0] == static_cast<std::uint64_t>(table_bits) && c[1] == static_cast<std::uint64_t>(slot_extra) &&
            c[2] == static_cast<std::uint64_t>(disc_slots))
            return c[3];
    std::size_t free_b = 0, total_b = 0;
    if (!check(cudaMemGetInfo(&free_b, &total_b), "cudaMemGetInfo")) return 0;
    const std::size_t cap = free_b / 10 * 9;
    hp::DevArena a{};
    std::uint64_t* d_used = nullptr;
    hp::DevArena* d_arena = nullptr;
    std::uint64_t used = 0;
    bool ok = check(cudaMalloc(&a.base, cap), "probe arena") &&
              check(cudaMemset(a.base, 0, cap), "probe arena") && (a.cap = cap, true) &&
              check(cudaMalloc(&d_arena, sizeof a), "probe") &&
              check(cudaMemcpy(d_arena, &a, sizeof a, cudaMemcpyHostToDevice), "probe") &&
              check(cudaMemcpyToSymbol(hp::hp_arenas, &d_arena, sizeof d_arena), "probe") &&
              check(cudaMalloc(&d_used, sizeof used), "probe");
    if (ok) {
        const auto t0 = std::chrono::steady_clock::now();
        k_probe<<<1, 1>>>(table_bits, slot_extra, disc_slots, d_used);
        ok = check(cudaGetLastError(), "probe launch") && check(cudaDeviceSynchronize(), "probe kernel") &&
             check(cudaMemcpy(&used, d_used, sizeof used, cudaMemcpyDeviceToHost), "probe");
        if (trace()) std::fprintf(stderr, "[gpu] probe bits=%d g=%d D=%d: %llu B arena, %.0f ms\n", table_bits, slot_extra,
                                  disc_slots, static_cast<unsigned long long>(used), ms_since(t0));
    }
    cudaFree(a.base); cudaFree(d_arena); cudaFree(d_used);
    if (!ok) return 0;
    const std::uint64_t bytes = (used + used / 16 + (std::uint64_t(1) << 20) + 255) & ~std::uint64_t(255);
    cache.push_back({static_cast<std::uint64_t>(table_bits), static_cast<std::uint64_t>(slot_extra),
                     static_cast<std::uint64_t>(disc_slots), bytes});
    return bytes;
}

// Runs every task on the GPU, as many at once as memory allows. False on a CUDA error.
bool run(std::vector<Task>& tasks, bool decode) {
    if (tasks.empty()) return true;
    if (!init()) return false;
    std::uint64_t arena = 0;
    for (const Task& t : tasks) {
        const std::uint64_t a = arena_bytes(t.table_bits, t.slot_extra, t.disc_slots);
        if (!a) return false;
        arena = std::max(arena, a);
    }
    std::size_t done = 0;
    while (done < tasks.size()) {
        std::size_t free_b = 0, total_b = 0;
        if (!check(cudaMemGetInfo(&free_b, &total_b), "cudaMemGetInfo")) return false;
        const std::uint64_t budget = free_b > (std::uint64_t(512) << 20) ? free_b - (std::uint64_t(512) << 20) : 0;
        // Shards in this wave: arena + input + output each.
        std::size_t wave = 0;
        std::uint64_t need = 0;
        for (std::size_t i = done; i < tasks.size(); ++i) {
            const std::uint64_t one = arena + ((tasks[i].in_len + 255) & ~std::uint64_t(255)) +
                                      ((tasks[i].out_cap + 255) & ~std::uint64_t(255)) + sizeof(Job) + sizeof(hp::DevArena);
            if (wave && need + one > budget) break;
            need += one;
            ++wave;
        }
        if (need > budget) { g_dev.error = "not enough GPU memory for one shard (try a smaller -m)"; return false; }

        std::uint64_t in_total = 0, out_total = 0;
        for (std::size_t i = 0; i < wave; ++i) {
            in_total += (tasks[done + i].in_len + 255) & ~std::uint64_t(255);
            out_total += (tasks[done + i].out_cap + 255) & ~std::uint64_t(255);
        }
        std::uint8_t *d_in = nullptr, *d_out = nullptr, *d_arena_mem = nullptr;
        Job* d_jobs = nullptr;
        hp::DevArena* d_arenas = nullptr;
        bool ok = check(cudaMalloc(&d_in, in_total ? in_total : 1), "GPU input buffer") &&
                  check(cudaMalloc(&d_out, out_total ? out_total : 1), "GPU output buffer") &&
                  check(cudaMalloc(&d_arena_mem, arena * wave), "GPU model memory") &&
                  check(cudaMemset(d_arena_mem, 0, arena * wave), "GPU model memory") &&
                  check(cudaMalloc(&d_jobs, sizeof(Job) * wave), "GPU jobs") &&
                  check(cudaMalloc(&d_arenas, sizeof(hp::DevArena) * wave), "GPU arenas");
        std::vector<Job> jobs(wave);
        std::vector<hp::DevArena> arenas(wave);
        if (ok) {
            std::uint64_t io = 0, oo = 0;
            for (std::size_t i = 0; i < wave && ok; ++i) {
                const Task& t = tasks[done + i];
                Job j{};
                j.in = d_in + io; j.in_len = t.in_len;
                j.out = d_out + oo; j.out_cap = t.out_cap;
                j.n = t.n;
                j.table_bits = t.table_bits; j.slot_extra = t.slot_extra; j.disc_slots = t.disc_slots;
                jobs[i] = j;
                arenas[i] = hp::DevArena{d_arena_mem + arena * i, 0, arena, 0};
                if (t.in_len) ok = check(cudaMemcpy(d_in + io, t.in, t.in_len, cudaMemcpyHostToDevice), "upload");
                io += (t.in_len + 255) & ~std::uint64_t(255);
                oo += (t.out_cap + 255) & ~std::uint64_t(255);
            }
            ok = ok && check(cudaMemcpy(d_jobs, jobs.data(), sizeof(Job) * wave, cudaMemcpyHostToDevice), "upload") &&
                 check(cudaMemcpy(d_arenas, arenas.data(), sizeof(hp::DevArena) * wave, cudaMemcpyHostToDevice), "upload") &&
                 check(cudaMemcpyToSymbol(hp::hp_arenas, &d_arenas, sizeof d_arenas), "upload");
        }
        // Threads per block. Shards diverge (different data, different branches), so
        // small blocks spread them over every SM instead of stalling one warp.
        static const int tpb_env = std::getenv("HPD_GPU_TPB") ? std::atoi(std::getenv("HPD_GPU_TPB")) : 0;
        const int tpb = tpb_env > 0 ? tpb_env : 1;   // measured: 1 > 2 > 4 > 8 > 16 > 32
        const int blocks = static_cast<int>((wave + tpb - 1) / tpb);
        if (trace()) std::fprintf(stderr, "[gpu] wave %zu shards, arena %llu B each, %s\n", wave,
                                  static_cast<unsigned long long>(arena), decode ? "decode" : "encode");
        if (ok) {
            const auto t0 = std::chrono::steady_clock::now();
            k_init<<<blocks, tpb>>>(d_jobs, static_cast<int>(wave), decode ? 1 : 0);
            ok = check(cudaGetLastError(), "launch") && check(cudaDeviceSynchronize(), "GPU model set-up");
            if (trace()) std::fprintf(stderr, "[gpu] init %.0f ms\n", ms_since(t0));
        }
        // Step in short launches; size each from the last one's time (~100 ms).
        std::uint64_t step = 64, max_n = 0;
        for (std::size_t i = 0; i < wave; ++i) max_n = std::max(max_n, tasks[done + i].n);
        std::vector<int> status(wave);
        for (std::uint64_t coded = 0; ok;) {
            const auto t0 = std::chrono::steady_clock::now();
            if (decode) k_decode<<<blocks, tpb>>>(d_jobs, static_cast<int>(wave), step);
            else k_encode<<<blocks, tpb>>>(d_jobs, static_cast<int>(wave), step);
            ok = check(cudaGetLastError(), "launch") && check(cudaDeviceSynchronize(), "GPU model loop");
            if (!ok) break;
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            const std::uint64_t counted = coded < max_n ? std::min(step, max_n - coded) : 0;
            coded += step;
            g_done += counted * wave;
            if (ms > 0) g_dev.ms_per_byte = ms / static_cast<double>(step);
            if (trace()) std::fprintf(stderr, "[gpu] step %llu B/shard: %.1f ms (%.1f KiB/s per shard, %.1f MiB/s all)\n",
                                      static_cast<unsigned long long>(step), ms, step / 1.024 / (ms > 0 ? ms : 1),
                                      step * static_cast<double>(wave) / 1048.576 / (ms > 0 ? ms : 1));
            if (coded >= max_n + 1) {
                // All bytes are coded once coded > max_n; check every shard finished.
                ok = check(cudaMemcpy(jobs.data(), d_jobs, sizeof(Job) * wave, cudaMemcpyDeviceToHost), "download");
                bool all = true;
                for (std::size_t i = 0; i < wave; ++i) if (jobs[i].status == 0) all = false;
                if (all || !ok) break;
            }
            const double target = 100.0;
            std::uint64_t next = ms > 1 ? static_cast<std::uint64_t>(static_cast<double>(step) * target / ms) : step * 4;
            next = std::max<std::uint64_t>(16, std::min<std::uint64_t>(next, step * 4));
            step = next;
        }
        if (ok) {
            arenas.assign(wave, hp::DevArena{});
            ok = check(cudaMemcpy(arenas.data(), d_arenas, sizeof(hp::DevArena) * wave, cudaMemcpyDeviceToHost), "download");
            for (std::size_t i = 0; i < wave && ok; ++i)
                if (arenas[i].overflow) { g_dev.error = "GPU shard ran out of model memory"; ok = false; }
        }
        for (std::size_t i = 0; i < wave && ok; ++i) {
            Task& t = tasks[done + i];
            const Job& j = jobs[i];
            t.gave_up = j.status == 2;
            const std::uint64_t len = decode ? t.n : (t.gave_up ? 0 : j.io);
            t.out->resize(static_cast<std::size_t>(len));
            if (len) ok = check(cudaMemcpy(t.out->data(), j.out, len, cudaMemcpyDeviceToHost), "download");
        }
        cudaFree(d_in); cudaFree(d_out); cudaFree(d_arena_mem); cudaFree(d_jobs); cudaFree(d_arenas);
        if (!ok) return false;
        done += wave;
    }
    return true;
}

}  // namespace gpu
