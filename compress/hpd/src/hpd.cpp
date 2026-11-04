// hpd — day-to-day context-mixing compressor built on HP40-Lite.
//
//   hpd c [-t threads] [-m table_bits] [-b block_MiB] [-x 0|1] <in> <out>
//   hpd d [-t threads] <in> <out>
//
// Differences from hp (HP40):
//   * independent blocks, compressed and decompressed on several threads;
//   * a block that does not shrink is stored raw;
//   * table size follows the block size (no 10 GiB for a 4 KB file), and
//     tables are demand-zero, so start-up is free;
//   * x86 CALL (E8) operands are made absolute when a block looks like code.
// The archive header carries everything the decoder needs.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "hp/predictor.hpp"

namespace {

constexpr char kMagic[4] = {'H', 'P', 'D', '1'};
constexpr int kVersion = 1;
#ifndef HPD_SLOT_EXTRA
#define HPD_SLOT_EXTRA 2
#endif
enum : std::uint8_t { kStored = 0, kCM = 1 };
enum : std::uint8_t { kFilterE8 = 1 };

// ---------------------------------------------------------------- arithmetic coder over memory
inline std::uint32_t split(std::uint32_t x1, std::uint32_t x2, int p) {
    const std::uint32_t range = x2 - x1;
    return x1 + (range >> 16) * static_cast<std::uint32_t>(p) +
           (((range & 0xffffu) * static_cast<std::uint32_t>(p)) >> 16);
}

struct MemEncoder {
    std::vector<std::uint8_t>& out;
    std::uint32_t x1 = 0, x2 = 0xffffffffu;
    explicit MemEncoder(std::vector<std::uint8_t>& o) : out(o) {}
    void encode(int bit, int p) {
        const std::uint32_t xmid = split(x1, x2, p);
        if (bit) x2 = xmid; else x1 = xmid + 1;
        while (((x1 ^ x2) & 0xff000000u) == 0) {
            out.push_back(static_cast<std::uint8_t>(x2 >> 24));
            x1 <<= 8; x2 = (x2 << 8) | 255u;
        }
    }
    void flush() { for (int i = 0; i < 4; ++i) { out.push_back(static_cast<std::uint8_t>(x1 >> 24)); x1 <<= 8; } }
};

struct MemDecoder {
    const std::uint8_t* p; const std::uint8_t* end;
    std::uint32_t x1 = 0, x2 = 0xffffffffu, x = 0;
    MemDecoder(const std::uint8_t* b, const std::uint8_t* e) : p(b), end(e) {
        for (int i = 0; i < 4; ++i) x = (x << 8) | next();
    }
    std::uint32_t next() { return p < end ? *p++ : 0u; }
    int decode(int pr) {
        const std::uint32_t xmid = split(x1, x2, pr);
        const int bit = x <= xmid ? 1 : 0;
        if (bit) x2 = xmid; else x1 = xmid + 1;
        while (((x1 ^ x2) & 0xff000000u) == 0) {
            x1 <<= 8; x2 = (x2 << 8) | 255u; x = (x << 8) | next();
        }
        return bit;
    }
};

// ---------------------------------------------------------------- x86 E8 transform
// For each E8 byte, the next 4 bytes (little-endian) are a CALL displacement.
// Displacements in [-2^24, 2^24) are rotated by the position mod 2^25, which
// turns repeated calls to one function into repeated bytes. It is a bijection
// on that range and leaves every other value alone, so it always inverts.
inline std::uint32_t rd32(const std::uint8_t* q) {
    return q[0] | (q[1] << 8) | (q[2] << 16) | (static_cast<std::uint32_t>(q[3]) << 24);
}
inline void wr32(std::uint8_t* q, std::uint32_t v) {
    q[0] = v & 255; q[1] = (v >> 8) & 255; q[2] = (v >> 16) & 255; q[3] = v >> 24;
}
inline bool small25(std::uint32_t v) { const std::uint32_t t = v >> 24; return t == 0 || t == 0xff; }
inline std::uint32_t rot25(std::uint32_t v, std::int64_t d) {
    std::int64_t x = static_cast<std::int32_t>(v);              // in [-2^24, 2^24)
    x = (x + (1 << 24) + d) % (1 << 25);
    if (x < 0) x += (1 << 25);
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(x - (1 << 24)));
}
void e8_apply(std::vector<std::uint8_t>& b, bool forward) {
    const std::size_t n = b.size();
    for (std::size_t i = 0; i + 5 <= n; ++i) {
        if (b[i] != 0xE8) continue;
        const std::uint32_t v = rd32(&b[i + 1]);
        if (small25(v)) {
            const std::int64_t d = static_cast<std::int64_t>(i % (1u << 25));
            wr32(&b[i + 1], rot25(v, forward ? d : -d));
        }
        i += 4;
    }
}
// Real code calls the same functions again and again: many E8 operands, and
// their absolute targets repeat. Text, images and compressed data do not.
bool looks_like_x86(const std::uint8_t* p, std::size_t n) {
    if (n < 4096) return false;
    std::size_t valid = 0, repeat = 0;
    std::unordered_set<std::uint32_t> seen;
    for (std::size_t i = 0; i + 5 <= n; ++i) {
        if (p[i] != 0xE8) continue;
        const std::uint32_t v = rd32(p + i + 1);
        if (small25(v)) {
            ++valid;
            const std::uint32_t abs = static_cast<std::uint32_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(v)) + static_cast<std::int64_t>(i));
            if (!seen.insert(abs).second) ++repeat;
        }
        i += 4;
    }
    return valid >= 64 && valid * 1000 >= n && repeat * 10 >= valid;
}

// ---------------------------------------------------------------- one block
struct Block {
    std::uint8_t method = kStored, filters = 0, table_bits = 16, slot_extra = 32;
    std::uint32_t raw = 0;
    std::vector<std::uint8_t> data;
};

int auto_table_bits(std::size_t n, int cap) {
    // Measured: with checksummed buckets, ratio flattens once each model's
    // table holds ~4x the block's bytes. 2^bits slots x 2 B per model.
    int b = 16;
    while (b < cap && (std::size_t(1) << b) < n * 4) ++b;
    return b;
}

void compress_block(const std::uint8_t* src, std::size_t n, int bits_cap, int force_bits, int x86_mode, int slot_extra, Block& out) {
    out.raw = static_cast<std::uint32_t>(n);
    std::vector<std::uint8_t> buf(src, src + n);
    const bool x86 = x86_mode == 1 || (x86_mode < 0 && looks_like_x86(src, n));
    if (x86) { e8_apply(buf, true); out.filters |= kFilterE8; }
    hp::Config cfg;
    cfg.table_bits = force_bits > 0 ? force_bits : auto_table_bits(n, bits_cap);
    cfg.normalize();
    cfg.slot_extra = slot_extra;
    out.table_bits = static_cast<std::uint8_t>(cfg.table_bits);
    out.slot_extra = static_cast<std::uint8_t>(slot_extra);
    hp::Predictor pred(cfg);
    out.data.clear();
    out.data.reserve(n / 3 + 64);
    MemEncoder enc(out.data);
    bool gave_up = false;
    for (std::size_t k = 0; k < n; ++k) {
        const int c = buf[k];
        for (int i = 7; i >= 0; --i) {
            const int p = pred.predict();
            const int bit = (c >> i) & 1;
            enc.encode(bit, hp::clamp_int(p << 4, 1, 65535));
            pred.update(bit);
        }
        // Give up early on data CM cannot shrink (already-compressed media):
        // after 1 MiB, if the output is still larger than the input so far.
        if ((k & 0xffff) == 0xffff && k >= (1u << 20) && out.data.size() > k) { gave_up = true; break; }
    }
    enc.flush();
    if (gave_up || out.data.size() >= n) {          // stored
        out.method = kStored; out.filters = 0;
        out.data.assign(src, src + n);
    } else {
        out.method = kCM;
    }
}

bool decompress_block(const Block& b, std::uint8_t* dst) {
    if (b.method == kStored) {
        if (b.data.size() != b.raw) return false;
        std::memcpy(dst, b.data.data(), b.raw);
        return true;
    }
    hp::Config cfg;
    cfg.table_bits = b.table_bits;
    cfg.normalize();
    cfg.slot_extra = b.slot_extra;
    hp::Predictor pred(cfg);
    MemDecoder dec(b.data.data(), b.data.data() + b.data.size());
    for (std::uint32_t k = 0; k < b.raw; ++k) {
        int byte = 0;
        for (int i = 0; i < 8; ++i) {
            const int bit = dec.decode(hp::clamp_int(pred.predict() << 4, 1, 65535));
            pred.update(bit);
            byte = (byte << 1) | bit;
        }
        dst[k] = static_cast<std::uint8_t>(byte);
    }
    if (b.filters & kFilterE8) {
        std::vector<std::uint8_t> t(dst, dst + b.raw);
        e8_apply(t, false);
        std::memcpy(dst, t.data(), b.raw);
    }
    return true;
}

// ---------------------------------------------------------------- io helpers
bool read_file(const char* path, std::vector<std::uint8_t>& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) { std::perror(path); return false; }
    std::fseek(f, 0, SEEK_END);
    const long long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<std::size_t>(n));
    const bool ok = n == 0 || std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}
void put(std::vector<std::uint8_t>& o, std::uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) o.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
std::uint64_t get(const std::uint8_t*& p, int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= static_cast<std::uint64_t>(*p++) << (8 * i);
    return v;
}

template <class F>
void parallel_for(std::size_t n, int threads, F f) {
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> pool;
    const int t = static_cast<int>(std::min<std::size_t>(n, static_cast<std::size_t>(std::max(1, threads))));
    for (int k = 0; k < t; ++k)
        pool.emplace_back([&] { for (std::size_t i; (i = next++) < n;) f(i); });
    for (auto& th : pool) th.join();
}

int usage() {
    std::fprintf(stderr,
        "usage: hpd c [-t threads] [-m table_bits 16..22] [-M cap_bits] [-g slot_extra] [-b block_MiB] [-x 0|1] <in> <out>\n"
        "       hpd d [-t threads] <in> <out>\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) return usage();
    const char* mode = argv[1];
    int threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    int force_bits = 0, bits_cap = 22, x86_mode = -1, slot_extra = HPD_SLOT_EXTRA;
    std::size_t block_mib = 0;      // 0 = auto
    int i = 2;
    for (; i < argc - 2; ++i) {
        if (!std::strcmp(argv[i], "-t") && i + 1 < argc - 2) threads = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "-m") && i + 1 < argc - 2) force_bits = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "-M") && i + 1 < argc - 2) bits_cap = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "-b") && i + 1 < argc - 2) block_mib = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "-x") && i + 1 < argc - 2) x86_mode = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "-g") && i + 1 < argc - 2) slot_extra = std::atoi(argv[++i]);
        else return usage();
    }
    const char* inp = argv[argc - 2];
    const char* outp = argv[argc - 1];
    const auto t0 = std::chrono::steady_clock::now();

    if (!std::strcmp(mode, "c")) {
        std::vector<std::uint8_t> in;
        if (!read_file(inp, in)) return 1;
        const std::size_t n = in.size();
        // Auto blocks: one block up to 16 MiB (splitting costs ratio: 8 MiB of
        // enwik8 is 2.7% larger as 2 blocks, 6% as 4); beyond that, enough
        // blocks to keep every thread busy, never smaller than 16 MiB.
        std::size_t bs = block_mib ? block_mib << 20 : 0;
        if (!bs) {
            const std::size_t min_bs = std::size_t(16) << 20;
            bs = n <= min_bs ? std::max<std::size_t>(n, 1)
                 : std::max<std::size_t>(min_bs, (n + threads - 1) / threads);
        }
        if (bs > 0xffffffffu) bs = 0xffffffffu;
        const std::size_t nb = n ? (n + bs - 1) / bs : 0;
        std::vector<Block> blocks(nb);
        parallel_for(nb, threads, [&](std::size_t k) {
            const std::size_t off = k * bs, len = std::min(bs, n - off);
            compress_block(in.data() + off, len, bits_cap, force_bits, x86_mode, slot_extra, blocks[k]);
        });
        std::vector<std::uint8_t> hdr;
        hdr.insert(hdr.end(), kMagic, kMagic + 4);
        hdr.push_back(kVersion);
        put(hdr, n, 8);
        put(hdr, nb, 4);
        std::FILE* f = std::fopen(outp, "wb");
        if (!f) { std::perror(outp); return 1; }
        std::fwrite(hdr.data(), 1, hdr.size(), f);
        std::size_t total = hdr.size();
        for (const Block& b : blocks) {
            std::vector<std::uint8_t> bh;
            bh.push_back(b.method); bh.push_back(b.filters); bh.push_back(b.table_bits); bh.push_back(b.slot_extra);
            put(bh, b.raw, 4); put(bh, b.data.size(), 4);
            std::fwrite(bh.data(), 1, bh.size(), f);
            std::fwrite(b.data.data(), 1, b.data.size(), f);
            total += bh.size() + b.data.size();
        }
        std::fclose(f);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        int stored = 0, x86 = 0;
        for (const Block& b : blocks) { stored += b.method == kStored; x86 += (b.filters & kFilterE8) != 0; }
        std::fprintf(stderr, "%zu -> %zu B (%.3f bpc), %zu block(s) [%d stored, %d x86], %.1f s, %.1f KiB/s\n",
                     n, total, n ? 8.0 * total / n : 0.0, nb, stored, x86, s, n / 1024.0 / (s > 0 ? s : 1));
        return 0;
    }
    if (!std::strcmp(mode, "d")) {
        std::vector<std::uint8_t> a;
        if (!read_file(inp, a)) return 1;
        if (a.size() < 17 || std::memcmp(a.data(), kMagic, 4) || a[4] != kVersion) {
            std::fprintf(stderr, "not an hpd v%d archive\n", kVersion); return 1;
        }
        const std::uint8_t* p = a.data() + 5;
        const std::uint8_t* end = a.data() + a.size();
        const std::uint64_t n = get(p, 8);
        const std::uint32_t nb = static_cast<std::uint32_t>(get(p, 4));
        std::vector<Block> blocks(nb);
        std::vector<std::size_t> offs(nb);
        std::size_t off = 0;
        for (std::uint32_t k = 0; k < nb; ++k) {
            if (end - p < 12) { std::fprintf(stderr, "truncated\n"); return 1; }
            Block& b = blocks[k];
            b.method = *p++; b.filters = *p++; b.table_bits = *p++; b.slot_extra = *p++;
            b.raw = static_cast<std::uint32_t>(get(p, 4));
            const std::size_t len = static_cast<std::size_t>(get(p, 4));
            if (static_cast<std::size_t>(end - p) < len) { std::fprintf(stderr, "truncated\n"); return 1; }
            b.data.assign(p, p + len); p += len;
            offs[k] = off; off += b.raw;
        }
        if (off != n) { std::fprintf(stderr, "size mismatch\n"); return 1; }
        std::vector<std::uint8_t> out(static_cast<std::size_t>(n));
        std::atomic<bool> ok{true};
        parallel_for(nb, threads, [&](std::size_t k) {
            if (!decompress_block(blocks[k], out.data() + offs[k])) ok = false;
        });
        if (!ok) { std::fprintf(stderr, "corrupt block\n"); return 1; }
        std::FILE* f = std::fopen(outp, "wb");
        if (!f) { std::perror(outp); return 1; }
        std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "decompressed %llu B, %.1f s\n", static_cast<unsigned long long>(n), s);
        return 0;
    }
    return usage();
}
