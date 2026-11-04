// hpd — day-to-day context-mixing compressor built on HP40-Lite.
//
//   hpd c [options] <in> [<out>]   compress     (default out: <in>.hpd; "-" = stdin/stdout)
//   hpd d [options] <in> [<out>]   decompress   (default out: <in> without .hpd)
//   hpd t <archive>...             test: decode and verify every checksum, write nothing
//   hpd l <archive>...             list blocks and settings
//
// Differences from hp (HP40):
//   * independent blocks, compressed and decompressed on several threads;
//   * a block that does not shrink is stored raw, and 64 KiB chunks of
//     already-compressed data inside a block are copied instead of modelled;
//   * table size follows the block size (no 10 GiB for a 4 KB file), and
//     tables are demand-zero, so start-up is free;
//   * x86 CALL (E8) operands are made absolute when a block looks like code;
//   * every block and the whole stream carry a CRC-32, and the container is
//     a stream (no sizes up front), so pipes work in both directions.
// The archive carries everything the decoder needs (format: README.md).

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shellapi.h>
#  include <fcntl.h>
#  include <io.h>
#else
#  include <unistd.h>
#endif

#include "hp/predictor.hpp"

namespace {

namespace fs = std::filesystem;

constexpr char kMagic[4] = {'H', 'P', 'D', '1'};
constexpr int kVersion = 3;
constexpr const char* kVersionText = "hpd 1.0.0 (writes archive format 3, reads 2 and 3)";
#ifndef HPD_SLOT_EXTRA
#define HPD_SLOT_EXTRA 2
#endif
enum : std::uint8_t { kStored = 0, kCM = 1, kEnd = 0xFF };
enum : std::uint8_t { kFilterE8 = 1 };

// Limits shared by encoder and decoder; the decoder rejects anything outside
// them before allocating, so a hostile archive cannot ask for 2^31-slot tables.
constexpr std::uint32_t kMaxBlock = 1u << 30;      // 1 GiB of raw data per block
constexpr int kMaxTableBits = 24;
constexpr int kMaxSlotExtra = 8;
constexpr int kChunkLog = 16;                       // pass-through granularity: 64 KiB

// ---------------------------------------------------------------- CRC-32 (IEEE, as zlib)
struct Crc32Tables {
    std::uint32_t t[8][256];
    Crc32Tables() {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
            t[0][i] = c;
        }
        for (int s = 1; s < 8; ++s)
            for (int i = 0; i < 256; ++i) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 255];
    }
};
const Crc32Tables& crc_tables() { static const Crc32Tables t; return t; }

std::uint32_t crc32(std::uint32_t crc, const std::uint8_t* p, std::size_t n) {
    const auto& t = crc_tables().t;
    crc = ~crc;
    while (n >= 8) {
        const std::uint32_t a = crc ^ (p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24));
        const std::uint32_t b = p[4] | (p[5] << 8) | (p[6] << 16) | (static_cast<std::uint32_t>(p[7]) << 24);
        crc = t[7][a & 255] ^ t[6][(a >> 8) & 255] ^ t[5][(a >> 16) & 255] ^ t[4][a >> 24] ^
              t[3][b & 255] ^ t[2][(b >> 8) & 255] ^ t[1][(b >> 16) & 255] ^ t[0][b >> 24];
        p += 8; n -= 8;
    }
    while (n--) crc = (crc >> 8) ^ t[0][(crc ^ *p++) & 255];
    return ~crc;
}

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

// ---------------------------------------------------------------- progress
std::atomic<std::uint64_t> g_done{0};      // raw bytes coded so far (all threads)

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
void e8_apply(std::uint8_t* b, std::size_t n, bool forward) {
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

// ---------------------------------------------------------------- incompressible chunks
// Already-compressed data (JPEG, zip, video, encrypted) gains nothing from the
// model and costs ~12 us a byte, so 64 KiB chunks of it are copied instead.
// A chunk is passed through when (a) its order-0 entropy and its entropy given
// the high nibble of each of the 4 previous bytes are all >= 7.9 bits/byte
// (Miller-Madow corrected), and (b) under 1 in 32 of its content-defined
// 64-byte fingerprints occur anywhere else in the block, in either direction:
// a repeat is something the match models would find. Only the encoder runs
// this; the decoder reads the decision from the block's chunk map.
constexpr std::size_t kMinJudged = 16384;   // a shorter tail chunk is always modelled
constexpr double kFlatBits = 7.9;

std::uint64_t splitmix64(std::uint64_t& s) {
    std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

double entropy_bits(const std::uint32_t* cnt, int contexts) {
    double bits = 0;
    std::uint64_t total = 0;
    for (int c = 0; c < contexts; ++c) {
        const std::uint32_t* h = cnt + 256 * c;
        std::uint64_t nc = 0;
        int k = 0;
        double s = 0;
        for (int x = 0; x < 256; ++x)
            if (h[x]) { nc += h[x]; ++k; s += h[x] * std::log2(static_cast<double>(h[x])); }
        if (!nc) continue;
        bits += static_cast<double>(nc) * std::log2(static_cast<double>(nc)) - s + (k - 1) / (2.0 * 0.6931471805599453);
        total += nc;
    }
    return total ? bits / static_cast<double>(total) : 0.0;
}

// One flag per 2^kChunkLog chunk (1 = pass through); empty if nothing qualifies.
std::vector<std::uint8_t> incompressible_chunks(const std::uint8_t* p, std::size_t n) {
    std::vector<std::uint8_t> flag;
    if (n < kMinJudged) return flag;
    const std::size_t cs = std::size_t(1) << kChunkLog;
    const std::size_t nc = (n + cs - 1) / cs;

    static const std::array<std::uint64_t, 256> gear = [] {
        std::array<std::uint64_t, 256> g{};
        std::uint64_t s = 0x6870642d67656172ull;
        for (auto& v : g) v = splitmix64(s);
        return g;
    }();
    std::vector<std::uint32_t> samples(nc, 0), repeats(nc, 0);
    std::size_t cap = 1024;
    while (cap < 4 * (n / 256 + 16)) cap <<= 1;
    std::vector<std::uint32_t> fp(cap, 0), owner(cap, 0);
    std::size_t used = 0;
    std::uint64_t h = 0;
    for (std::size_t i = 0; i < n; ++i) {
        h = (h << 1) + gear[p[i]];                  // top bits depend on the last 64 bytes
        if ((h >> 56) != 0 || i < 63) continue;     // ~1 position in 256, chosen by content
        std::uint32_t f = static_cast<std::uint32_t>(h ^ (h >> 32));
        if (!f) f = 1;
        const std::uint32_t c = static_cast<std::uint32_t>(i >> kChunkLog);
        ++samples[c];
        std::size_t k = (static_cast<std::size_t>(f) * 0x9E3779B1u) & (cap - 1);
        while (fp[k] && fp[k] != f) k = (k + 1) & (cap - 1);
        if (fp[k] == f) {
            ++repeats[c];
            if (owner[k] != c) ++repeats[owner[k]];
        } else if (2 * used < cap) {
            fp[k] = f; owner[k] = c; ++used;
        }
    }

    flag.assign(nc, 0);
    bool any = false;
    std::vector<std::uint32_t> c0(256), cd(4 * 16 * 256);
    for (std::size_t c = 0; c < nc; ++c) {
        const std::size_t b = c * cs, e = std::min(n, b + cs);
        if (e - b < kMinJudged) continue;
        if (repeats[c] && repeats[c] * 32 >= samples[c]) continue;
        std::fill(c0.begin(), c0.end(), 0u);
        std::fill(cd.begin(), cd.end(), 0u);
        for (std::size_t i = b; i < e; ++i) {
            const int x = p[i];
            ++c0[x];
            if (i >= 4) {
                ++cd[0 * 4096 + (p[i - 1] >> 4) * 256 + x];
                ++cd[1 * 4096 + (p[i - 2] >> 4) * 256 + x];
                ++cd[2 * 4096 + (p[i - 3] >> 4) * 256 + x];
                ++cd[3 * 4096 + (p[i - 4] >> 4) * 256 + x];
            }
        }
        if (entropy_bits(c0.data(), 1) < kFlatBits) continue;
        bool flat = true;
        for (int d = 0; d < 4 && flat; ++d)
            if (entropy_bits(&cd[static_cast<std::size_t>(d) * 4096], 16) < kFlatBits) flat = false;
        if (flat) { flag[c] = 1; any = true; }
    }
    if (!any) flag.clear();
    return flag;
}

// ---------------------------------------------------------------- one block
struct Block {
    std::uint8_t method = kStored, filters = 0, table_bits = 0, slot_extra = 0, disc_slots = 0, chunk_log = 0;
    std::uint32_t raw = 0, crc = 0;
    bool has_crc = false;
    std::vector<std::uint8_t> map;    // pass-through flags, one bit per chunk (chunk_log != 0)
    std::vector<std::uint8_t> data;   // payload: CM stream, then the passed-through chunks
    std::uint64_t passed = 0;         // bytes passed through (statistics)
};

struct Settings {
    int bits_cap = 22, force_bits = 0, x86_mode = -1, slot_extra = HPD_SLOT_EXTRA, disc_slots = HP_DISC_SLOTS;
    bool bypass = true;
};

int auto_table_bits(std::size_t n, int cap) {
    // Measured: with checksummed buckets, ratio flattens once each model's
    // table holds ~4x the block's bytes. 2^bits slots x 2 B per model.
    int b = 16;
    while (b < cap && (std::size_t(1) << b) < n * 4) ++b;
    return b;
}

inline bool map_bit(const std::vector<std::uint8_t>& m, std::size_t c) { return (m[c >> 3] >> (c & 7)) & 1; }

void compress_block(const std::uint8_t* src, std::size_t n, const Settings& s, Block& out) {
    out = Block();
    out.raw = static_cast<std::uint32_t>(n);
    out.crc = crc32(0, src, n);
    out.has_crc = true;
    auto store = [&] {
        out.method = kStored; out.filters = 0; out.table_bits = 0; out.slot_extra = 0; out.disc_slots = 0;
        out.chunk_log = 0; out.map.clear(); out.passed = 0;
        out.data.assign(src, src + n);
    };
    if (n == 0) { store(); return; }

    std::vector<std::uint8_t> filtered;
    const std::uint8_t* buf = src;
    if (s.x86_mode == 1 || (s.x86_mode < 0 && looks_like_x86(src, n))) {
        filtered.assign(src, src + n);
        e8_apply(filtered.data(), n, true);
        buf = filtered.data();
        out.filters |= kFilterE8;
    }

    // Chunks to pass through; the model sees the other chunks back to back.
    const std::size_t cs = std::size_t(1) << kChunkLog;
    std::vector<std::uint8_t> skip = s.bypass ? incompressible_chunks(buf, n) : std::vector<std::uint8_t>();
    std::vector<std::uint8_t> kept_buf, tail;
    const std::uint8_t* kept = buf;
    std::size_t kept_n = n;
    if (!skip.empty()) {
        kept_buf.reserve(n);
        for (std::size_t c = 0; c < skip.size(); ++c) {
            const std::size_t b = c * cs, e = std::min(n, b + cs);
            auto& dst = skip[c] ? tail : kept_buf;
            dst.insert(dst.end(), buf + b, buf + e);
        }
        kept = kept_buf.data();
        kept_n = kept_buf.size();
        out.passed = tail.size();
        g_done += tail.size();
        if (kept_n == 0) { store(); return; }
    }

    hp::Config cfg;
    cfg.table_bits = s.force_bits > 0 ? s.force_bits : auto_table_bits(kept_n, s.bits_cap);
    cfg.normalize();
    cfg.slot_extra = s.slot_extra;
    cfg.disc_slots = s.disc_slots;
    out.disc_slots = static_cast<std::uint8_t>(s.disc_slots);
    out.table_bits = static_cast<std::uint8_t>(cfg.table_bits);
    out.slot_extra = static_cast<std::uint8_t>(s.slot_extra);
    hp::Predictor pred(cfg);
    out.data.clear();
    out.data.reserve(kept_n / 3 + 64);
    MemEncoder enc(out.data);
    bool gave_up = false;
    std::size_t k = 0;
    for (; k < kept_n; ++k) {
        const int c = kept[k];
        for (int i = 7; i >= 0; --i) {
            const int p = pred.predict();
            const int bit = (c >> i) & 1;
            enc.encode(bit, hp::clamp_int(p << 4, 1, 65535));
            pred.update(bit);
        }
        if ((k & 0xffff) == 0xffff) {
            g_done += 0x10000;
            // Give up early on data CM cannot shrink (already-compressed media):
            // after 1 MiB, if the output is still larger than the input so far.
            if (k >= (1u << 20) && out.data.size() > k) { gave_up = true; ++k; break; }
        }
    }
    g_done += k & 0xffff;
    enc.flush();
    if (gave_up || out.data.size() + tail.size() >= n) {
        g_done += n - k - out.passed;
        store();
        return;
    }
    out.method = kCM;
    if (!skip.empty()) {
        out.chunk_log = kChunkLog;
        out.map.assign((skip.size() + 7) / 8, 0);
        for (std::size_t c = 0; c < skip.size(); ++c)
            if (skip[c]) out.map[c >> 3] |= static_cast<std::uint8_t>(1u << (c & 7));
        out.data.insert(out.data.end(), tail.begin(), tail.end());
    }
}

// Bytes passed through in a block of `raw` bytes with this chunk map.
std::uint64_t passed_bytes(const Block& b) {
    if (!b.chunk_log) return 0;
    const std::uint64_t cs = std::uint64_t(1) << b.chunk_log;
    const std::uint64_t nc = (b.raw + cs - 1) / cs;
    std::uint64_t t = 0;
    for (std::uint64_t c = 0; c < nc; ++c)
        if (map_bit(b.map, c)) t += std::min<std::uint64_t>(cs, b.raw - c * cs);
    return t;
}

// Decodes one block into dst[0..raw). Returns an error message, or "" on success.
std::string decompress_block(const Block& b, std::uint8_t* dst) {
    if (b.method == kStored) {
        if (b.data.size() != b.raw) return "stored block has the wrong size";
        if (b.raw) std::memcpy(dst, b.data.data(), b.raw);
        g_done += b.raw;
    } else {
        const std::uint64_t tail = passed_bytes(b);
        if (tail > b.data.size()) return "chunk map does not match the payload";
        const std::size_t cm_len = b.data.size() - static_cast<std::size_t>(tail);
        hp::Config cfg;
        cfg.table_bits = b.table_bits;
        cfg.normalize();
        cfg.slot_extra = b.slot_extra;
        cfg.disc_slots = b.disc_slots;
        hp::Predictor pred(cfg);
        MemDecoder dec(b.data.data(), b.data.data() + cm_len);
        std::uint32_t since = 0;
        auto decode_span = [&](std::uint8_t* d, std::size_t len) {
            for (std::size_t k = 0; k < len; ++k) {
                int byte = 0;
                for (int i = 0; i < 8; ++i) {
                    const int bit = dec.decode(hp::clamp_int(pred.predict() << 4, 1, 65535));
                    pred.update(bit);
                    byte = (byte << 1) | bit;
                }
                d[k] = static_cast<std::uint8_t>(byte);
                if (++since == 0x10000) { g_done += since; since = 0; }
            }
        };
        if (!b.chunk_log) {
            decode_span(dst, b.raw);
        } else {
            const std::size_t cs = std::size_t(1) << b.chunk_log;
            const std::uint8_t* raw_part = b.data.data() + cm_len;
            for (std::size_t off = 0, c = 0; off < b.raw; off += cs, ++c) {
                const std::size_t len = std::min<std::size_t>(cs, b.raw - off);
                if (map_bit(b.map, c)) { std::memcpy(dst + off, raw_part, len); raw_part += len; g_done += len; }
                else decode_span(dst + off, len);
            }
        }
        g_done += since;
        if (b.filters & kFilterE8) e8_apply(dst, b.raw, false);
    }
    if (b.has_crc && crc32(0, dst, b.raw) != b.crc) return "CRC mismatch (archive is damaged)";
    return "";
}

// ---------------------------------------------------------------- platform helpers
#if defined(_WIN32)
std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
fs::path to_path(const std::string& s) { return fs::path(widen(s)); }
std::FILE* open_file(const std::string& name, bool write) {
    return _wfopen(widen(name).c_str(), write ? L"wb" : L"rb");
}
bool is_tty(std::FILE* f) { return _isatty(_fileno(f)) != 0; }
void binary_stdio() { _setmode(_fileno(stdin), _O_BINARY); _setmode(_fileno(stdout), _O_BINARY); }
bool seek_forward(std::FILE* f, std::uint64_t n) { return _fseeki64(f, static_cast<__int64>(n), SEEK_CUR) == 0; }
#else
fs::path to_path(const std::string& s) { return fs::path(s); }
std::FILE* open_file(const std::string& name, bool write) { return std::fopen(name.c_str(), write ? "wb" : "rb"); }
bool is_tty(std::FILE* f) { return isatty(fileno(f)) != 0; }
void binary_stdio() {}
bool seek_forward(std::FILE* f, std::uint64_t n) { return fseeko(f, static_cast<off_t>(n), SEEK_CUR) == 0; }
#endif

// Physical memory free for new allocations, or 0 if unknown.
std::uint64_t available_memory() {
#if defined(_WIN32)
    MEMORYSTATUSEX m;
    m.dwLength = sizeof m;
    return GlobalMemoryStatusEx(&m) ? m.ullAvailPhys : 0;
#elif defined(__linux__)
    std::FILE* f = std::fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[256];
    unsigned long long kb = 0;
    while (std::fgets(line, sizeof line, f))
        if (std::sscanf(line, "MemAvailable: %llu kB", &kb) == 1) break;
    std::fclose(f);
    return static_cast<std::uint64_t>(kb) << 10;
#else
    const long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGE_SIZE);
    return pages > 0 && size > 0 ? static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(size) / 2 : 0;
#endif
}

// Peak memory of one block job. The model's tables are demand-zero, but a
// block of 1 MiB or more touches nearly all of them: measured 732 MiB at
// table_bits 22 / slot_extra 2, halving per bit below and growing about 2x
// per extra growth bit above 2. Plus about three copies of the block.
std::uint64_t job_memory(int table_bits, int slot_extra, std::uint64_t block) {
    double t = 732.0 * std::ldexp(1.0, table_bits - 22) * std::ldexp(1.0, std::max(0, slot_extra - 2));
    return static_cast<std::uint64_t>(t * 1048576.0) + 3 * block + (16u << 20);
}

void put(std::vector<std::uint8_t>& o, std::uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) o.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
std::uint64_t get(const std::uint8_t*& p, int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= static_cast<std::uint64_t>(*p++) << (8 * i);
    return v;
}
std::size_t read_full(std::FILE* f, std::uint8_t* p, std::size_t n) {
    std::size_t got = 0;
    while (got < n) {
        const std::size_t r = std::fread(p + got, 1, n - got, f);
        if (r == 0) break;
        got += r;
    }
    return got;
}

// Runs f(0..n-1) on up to `threads` threads; false if any call threw.
template <class F>
bool parallel_for(std::size_t n, int threads, F f) {
    std::atomic<std::size_t> next{0};
    std::atomic<bool> ok{true};
    auto work = [&] {
        for (std::size_t i; (i = next++) < n;) {
            try { f(i); } catch (...) { ok = false; }
        }
    };
    const int t = static_cast<int>(std::min<std::size_t>(n, static_cast<std::size_t>(std::max(1, threads))));
    if (t <= 1) { work(); return ok; }
    std::vector<std::thread> pool;
    for (int k = 0; k < t; ++k) pool.emplace_back(work);
    for (auto& th : pool) th.join();
    return ok;
}

// Prints "\r  12.3 / 100.0 MiB  45%  88 KiB/s" every half second while running.
class Progress {
 public:
    Progress(bool on, std::uint64_t total) : on_(on), total_(total) {
        g_done = 0;
        if (on_) th_ = std::thread([this] { run(); });
    }
    ~Progress() { stop(); }
    void stop() {
        if (!on_) return;
        stop_ = true;
        th_.join();
        on_ = false;
        std::fprintf(stderr, "\r%60s\r", "");
    }
 private:
    void run() {
        const auto t0 = std::chrono::steady_clock::now();
        while (!stop_) {
            for (int i = 0; i < 10 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (stop_) break;
            const double done = static_cast<double>(g_done.load());
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (total_)
                std::fprintf(stderr, "\r  %.1f / %.1f MiB  %3.0f%%  %.0f KiB/s   ", done / 1048576.0, total_ / 1048576.0,
                             100.0 * done / static_cast<double>(total_), done / 1024.0 / (s > 0 ? s : 1));
            else
                std::fprintf(stderr, "\r  %.1f MiB  %.0f KiB/s   ", done / 1048576.0, done / 1024.0 / (s > 0 ? s : 1));
            std::fflush(stderr);
        }
    }
    bool on_;
    std::uint64_t total_;
    std::atomic<bool> stop_{false};
    std::thread th_;
};

// ---------------------------------------------------------------- archive reading (formats 2 and 3)
class Reader {
 public:
    explicit Reader(std::FILE* f) : f_(f) {}

    // Reads the file header. Returns "" or an error.
    std::string open() {
        std::uint8_t h[6];
        if (read_full(f_, h, 6) != 6 || std::memcmp(h, kMagic, 4)) return "not an hpd archive";
        version_ = h[4];
        if (version_ == 2) {
            std::uint8_t r[11];
            if (read_full(f_, r, 11) != 11) return "truncated archive";
            std::uint8_t full[12];
            full[0] = h[5];
            std::memcpy(full + 1, r, 11);
            const std::uint8_t* q = full;
            v2_total_ = get(q, 8);
            v2_blocks_ = get(q, 4);
            return "";
        }
        if (version_ != 3) return "unsupported archive version " + std::to_string(version_) + " (this hpd reads 2 and 3)";
        if (h[5] != 0) return "unknown archive flags (made by a newer hpd?)";
        return "";
    }
    int version() const { return version_; }

    // Next block. Sets end=true at the end of the archive. skip_payload: seek over it.
    std::string next(Block& b, bool& end, bool skip_payload = false) {
        end = false;
        b = Block();
        std::uint32_t len = 0;
        if (version_ == 2) {
            if (blocks_read_ == v2_blocks_) {
                end = true;
                if (total_ != v2_total_) return "size mismatch: header says " + std::to_string(v2_total_) + ", blocks hold " + std::to_string(total_);
                return "";
            }
            std::uint8_t bh[13];
            if (read_full(f_, bh, 13) != 13) return "truncated archive";
            b.method = bh[0]; b.filters = bh[1]; b.table_bits = bh[2]; b.slot_extra = bh[3]; b.disc_slots = bh[4];
            const std::uint8_t* q = bh + 5;
            b.raw = static_cast<std::uint32_t>(get(q, 4));
            len = static_cast<std::uint32_t>(get(q, 4));
        } else {
            std::uint8_t m;
            if (read_full(f_, &m, 1) != 1) return "truncated archive (no end marker)";
            if (m == kEnd) {
                std::uint8_t e[12];
                if (read_full(f_, e, 12) != 12) return "truncated archive";
                const std::uint8_t* q = e;
                const std::uint64_t total = get(q, 8);
                stream_crc_ = static_cast<std::uint32_t>(get(q, 4));
                end = true;
                if (total != total_) return "size mismatch: end marker says " + std::to_string(total) + ", blocks hold " + std::to_string(total_);
                return "";
            }
            std::uint8_t bh[17];
            if (read_full(f_, bh, 17) != 17) return "truncated archive";
            b.method = m; b.filters = bh[0]; b.table_bits = bh[1]; b.slot_extra = bh[2]; b.disc_slots = bh[3]; b.chunk_log = bh[4];
            const std::uint8_t* q = bh + 5;
            b.raw = static_cast<std::uint32_t>(get(q, 4));
            len = static_cast<std::uint32_t>(get(q, 4));
            b.crc = static_cast<std::uint32_t>(get(q, 4));
            b.has_crc = true;
        }
        // Reject impossible headers before allocating anything.
        bool ok = b.raw <= kMaxBlock;
        if (b.method == kStored) {
            ok = ok && len == b.raw && b.filters == 0 && b.chunk_log == 0;
        } else if (b.method == kCM) {
            ok = ok && b.raw > 0 && len <= b.raw && (b.filters & ~kFilterE8) == 0 && b.table_bits >= 16 &&
                 b.table_bits <= kMaxTableBits && b.slot_extra <= kMaxSlotExtra && b.disc_slots >= 1 &&
                 b.disc_slots <= 12 && (b.chunk_log == 0 || (b.chunk_log >= 12 && b.chunk_log <= 24));
        } else {
            ok = false;
        }
        if (!ok) return "corrupt block header";
        if (b.chunk_log) {
            const std::uint64_t cs = std::uint64_t(1) << b.chunk_log;
            const std::uint64_t nc = (b.raw + cs - 1) / cs;
            b.map.resize(static_cast<std::size_t>((nc + 7) / 8));
            if (read_full(f_, b.map.data(), b.map.size()) != b.map.size()) return "truncated archive";
            if ((nc & 7) && (b.map.back() >> (nc & 7))) return "corrupt chunk map";
            b.passed = passed_bytes(b);
            if (b.passed > len) return "corrupt chunk map";
        }
        if (skip_payload) {
            if (!seek_forward(f_, len)) {
                std::vector<std::uint8_t> tmp(1 << 16);
                for (std::uint64_t left = len; left;) {
                    const std::size_t k = static_cast<std::size_t>(std::min<std::uint64_t>(left, tmp.size()));
                    if (read_full(f_, tmp.data(), k) != k) return "truncated archive";
                    left -= k;
                }
            }
            b.data.clear();
            payload_ = len;
        } else {
            b.data.resize(len);
            if (len && read_full(f_, b.data.data(), len) != len) return "truncated archive";
        }
        total_ += b.raw;
        ++blocks_read_;
        return "";
    }
    // After end (format 3): CRC-32 of all data, and whether bytes follow the end marker.
    std::uint32_t stream_crc() const { return stream_crc_; }
    bool trailing_data() { return std::fgetc(f_) != EOF; }
    std::uint64_t payload_len() const { return payload_; }

 private:
    std::FILE* f_;
    int version_ = 0;
    std::uint64_t v2_total_ = 0, v2_blocks_ = 0, blocks_read_ = 0, total_ = 0, payload_ = 0;
    std::uint32_t stream_crc_ = 0;
};

// ---------------------------------------------------------------- options and commands
struct Options {
    int threads = 0;                 // 0 = auto
    bool threads_set = false;
    std::uint64_t block = 0;         // bytes, 0 = auto
    Settings s;
    bool to_stdout = false, force = false, quiet = false, verbose = false;
};

void usage(std::FILE* o) {
    std::fprintf(o,
        "%s\n"
        "usage: hpd c [options] <in> [<out>]   compress   (out defaults to <in>.hpd)\n"
        "       hpd d [options] <in> [<out>]   decompress (out defaults to <in> without .hpd)\n"
        "       hpd t <archive>...             test archives (decode and verify, write nothing)\n"
        "       hpd l <archive>...             list blocks and settings\n"
        "A file name of - means stdin / stdout.\n"
        "options:\n"
        "  -t N       threads (default: all cores, fewer if memory is short)\n"
        "  -9         stronger: 12 discovery slots (about 0.4%% smaller, 25%% slower)\n"
        "  -c         write to stdout\n"
        "  -f         overwrite an existing output file chosen by default name\n"
        "  -q / -v    quiet / verbose (one line per block)\n"
        "  -b MiB     block size (default: whole file up to 16 MiB, then 16..256 MiB per thread)\n"
        "  -B KiB     block size in KiB\n"
        "  -s 0|1     pass already-compressed 64 KiB chunks through unmodelled (default 1)\n"
        "  -x 0|1     force the x86 CALL transform off / on (default: detect per block)\n"
        "  -m bits    table size, 2^bits slots per model, 16..24 (default: about 4x the block)\n"
        "  -M bits    largest automatic table size, 16..24 (default 22)\n"
        "  -g bits    how far one model's table may grow above -m, 0..8 (default 2)\n"
        "  -D slots   discovery slots, 1..12 (default 4)\n"
        "  -h, -V     help, version\n",
        kVersionText);
}

bool parse_num(const std::string& s, long long lo, long long hi, long long& v) {
    if (s.empty()) return false;
    char* e = nullptr;
    const long long x = std::strtoll(s.c_str(), &e, 10);
    if (*e || x < lo || x > hi) return false;
    v = x;
    return true;
}

double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// Output sink: a file (removed again on failure), stdout, or nothing (test).
class Output {
 public:
    std::string open(const std::string& name, bool null_sink) {
        name_ = name;
        if (null_sink) return "";
        if (name == "-") { f_ = stdout; return ""; }
        f_ = open_file(name, true);
        if (!f_) return name + ": " + std::strerror(errno);
        created_ = true;
        return "";
    }
    bool write(const std::uint8_t* p, std::size_t n) {
        if (!f_ || !n) return true;
        if (std::fwrite(p, 1, n, f_) != n) ok_ = false;
        return ok_;
    }
    bool write(const std::vector<std::uint8_t>& v) { return write(v.data(), v.size()); }
    // Closes; on failure (or if !success) deletes a file we created.
    bool close(bool success) {
        bool ok = ok_;
        if (f_ && f_ != stdout) ok = std::fclose(f_) == 0 && ok;
        else if (f_) ok = std::fflush(f_) == 0 && ok;
        f_ = nullptr;
        if ((!success || !ok) && created_) {
            std::error_code ec;
            fs::remove(to_path(name_), ec);
        }
        return ok;
    }
 private:
    std::FILE* f_ = nullptr;
    std::string name_;
    bool ok_ = true, created_ = false;
};

int fail(const std::string& msg) {
    std::fprintf(stderr, "hpd: %s\n", msg.c_str());
    return 1;
}

std::string human(std::uint64_t n) {
    char b[64];
    if (n < 10000) std::snprintf(b, sizeof b, "%llu B", static_cast<unsigned long long>(n));
    else if (n < (10u << 20)) std::snprintf(b, sizeof b, "%.1f KiB", n / 1024.0);
    else if (n < (10ull << 30)) std::snprintf(b, sizeof b, "%.1f MiB", n / 1048576.0);
    else std::snprintf(b, sizeof b, "%.2f GiB", n / 1073741824.0);
    return b;
}

int do_compress(const std::string& in, const std::string& out, Options o) {
    const auto t0 = std::chrono::steady_clock::now();
    std::FILE* fin = stdin;
    std::uint64_t n_known = 0;
    bool size_known = false;
    if (in != "-") {
        std::error_code ec;
        const fs::path p = to_path(in);
        if (fs::is_directory(p, ec)) return fail(in + ": is a directory (archive it first, e.g. tar cf - dir | hpd c - out.hpd)");
        if (fs::is_regular_file(p, ec)) {
            n_known = fs::file_size(p, ec);
            size_known = !ec;
        }
        fin = open_file(in, false);
        if (!fin) return fail(in + ": " + std::strerror(errno));
    }

    // Threads and block size. Auto blocks: one block up to 16 MiB (splitting
    // costs ratio: 8 MiB of enwik8 is 2.7% larger as 2 blocks, 6% as 4);
    // beyond that, enough blocks to keep every thread busy, 16..256 MiB each.
    // Without a size (stdin) blocks are 16 MiB. Auto threads are then cut
    // until threads x job memory fits in 3/4 of the free RAM.
    const int hw = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    int threads = o.threads_set ? o.threads : hw;
    const std::uint64_t min_bs = std::uint64_t(16) << 20, max_bs = std::uint64_t(256) << 20;
    auto pick_block = [&](int t) -> std::uint64_t {
        if (o.block) return o.block;
        if (!size_known) return min_bs;
        if (n_known <= min_bs) return std::max<std::uint64_t>(n_known, 1);
        return std::min(max_bs, std::max(min_bs, (n_known + t - 1) / t));
    };
    std::uint64_t bs = pick_block(threads);
    const std::uint64_t avail = available_memory();
    const int bits_for_mem = o.s.force_bits ? o.s.force_bits : o.s.bits_cap;
    if (size_known) threads = static_cast<int>(std::min<std::uint64_t>(threads, std::max<std::uint64_t>(1, (n_known + bs - 1) / bs)));
    if (avail && !o.threads_set) {
        for (int it = 0; it < 8; ++it) {
            const std::uint64_t per = job_memory(bits_for_mem, o.s.slot_extra, bs);
            const int fit = static_cast<int>(std::max<std::uint64_t>(1, avail / 4 * 3 / per));
            if (fit >= threads) break;
            threads = fit;
            bs = pick_block(threads);
        }
    } else if (avail && !o.quiet) {
        const std::uint64_t need = job_memory(bits_for_mem, o.s.slot_extra, bs) * static_cast<std::uint64_t>(threads);
        if (need > avail)
            std::fprintf(stderr, "hpd: warning: %d threads may need %s, only %s free\n", threads, human(need).c_str(), human(avail).c_str());
    }
    if (bs > kMaxBlock) bs = kMaxBlock;

    Output w;
    if (std::string e = w.open(out, false); !e.empty()) { if (fin != stdin) std::fclose(fin); return fail(e); }
    std::vector<std::uint8_t> hdr(kMagic, kMagic + 4);
    hdr.push_back(kVersion);
    hdr.push_back(0);                                   // flags
    w.write(hdr);
    std::uint64_t total_in = 0, total_out = hdr.size(), passed = 0;
    std::uint32_t stream_crc = 0;
    int stored = 0, x86 = 0;
    Progress progress(!o.quiet && is_tty(stderr), size_known ? n_known : 0);

    const std::size_t batch = static_cast<std::size_t>(std::max(1, threads));
    std::vector<std::vector<std::uint8_t>> raw(batch);
    std::vector<Block> blocks(batch);
    bool eof = false, ok = true;
    std::string err;
    std::size_t block_no = 0;
    while (!eof && ok) {
        std::size_t cnt = 0;
        for (; cnt < batch; ++cnt) {
            raw[cnt].resize(static_cast<std::size_t>(bs));
            const std::size_t got = read_full(fin, raw[cnt].data(), raw[cnt].size());
            raw[cnt].resize(got);
            if (got < bs) { eof = true; if (got == 0) break; ++cnt; break; }
        }
        if (std::ferror(fin)) { ok = false; err = (in == "-" ? std::string("stdin") : in) + ": read error"; break; }
        if (!cnt) break;
        if (!parallel_for(cnt, threads, [&](std::size_t j) { compress_block(raw[j].data(), raw[j].size(), o.s, blocks[j]); })) {
            ok = false; err = "out of memory (try fewer threads with -t, or smaller blocks with -b)"; break;
        }
        for (std::size_t j = 0; j < cnt; ++j, ++block_no) {
            const Block& b = blocks[j];
            std::vector<std::uint8_t> bh;
            bh.push_back(b.method); bh.push_back(b.filters); bh.push_back(b.table_bits);
            bh.push_back(b.slot_extra); bh.push_back(b.disc_slots); bh.push_back(b.chunk_log);
            put(bh, b.raw, 4); put(bh, b.data.size(), 4); put(bh, b.crc, 4);
            bh.insert(bh.end(), b.map.begin(), b.map.end());
            w.write(bh);
            w.write(b.data);
            stream_crc = crc32(stream_crc, raw[j].data(), raw[j].size());
            total_in += b.raw;
            total_out += bh.size() + b.data.size();
            passed += b.passed;
            stored += b.method == kStored; x86 += (b.filters & kFilterE8) != 0;
            if (o.verbose)
                std::fprintf(stderr, "\rblock %zu: %u -> %zu B, %s%s, bits %d%s%s\n", block_no, b.raw, b.data.size(),
                             b.method == kCM ? "CM" : "stored", (b.filters & kFilterE8) ? "+x86" : "", b.table_bits,
                             b.passed ? ", passed through " : "", b.passed ? human(b.passed).c_str() : "");
            std::vector<std::uint8_t>().swap(blocks[j].data);
        }
        if (!w.write(nullptr, 0)) { ok = false; err = out + ": write error"; }
    }
    progress.stop();
    if (fin != stdin) std::fclose(fin);
    if (ok) {
        std::vector<std::uint8_t> end;
        end.push_back(kEnd);
        put(end, total_in, 8);
        put(end, stream_crc, 4);
        w.write(end);
        total_out += end.size();
    }
    if (!w.close(ok)) return fail(err.empty() ? (out == "-" ? std::string("stdout") : out) + ": write error" : err);
    if (!ok) return fail(err);
    const double s = seconds_since(t0);
    if (!o.quiet) {
        std::fprintf(stderr, "%s: %llu -> %llu B (%.3f bpc), %zu block%s", in == "-" ? "stdin" : in.c_str(),
                     static_cast<unsigned long long>(total_in), static_cast<unsigned long long>(total_out),
                     total_in ? 8.0 * static_cast<double>(total_out) / static_cast<double>(total_in) : 0.0, block_no,
                     block_no == 1 ? "" : "s");
        if (stored || x86) std::fprintf(stderr, " [%d stored, %d x86]", stored, x86);
        if (passed) std::fprintf(stderr, ", %s passed through", human(passed).c_str());
        std::fprintf(stderr, ", %d thread%s, %.1f s, %.1f KiB/s\n", threads, threads == 1 ? "" : "s", s,
                     static_cast<double>(total_in) / 1024.0 / (s > 0 ? s : 1));
    }
    return 0;
}

// Decompress (sink = file/stdout) or test (null sink).
int do_decompress(const std::string& in, const std::string& out, const Options& o, bool test) {
    const auto t0 = std::chrono::steady_clock::now();
    std::FILE* fin = stdin;
    std::uint64_t arc_size = 0;
    if (in != "-") {
        std::error_code ec;
        arc_size = fs::file_size(to_path(in), ec);
        if (ec) arc_size = 0;
        fin = open_file(in, false);
        if (!fin) return fail(in + ": " + std::strerror(errno));
    }
    const std::string label = in == "-" ? "stdin" : in;
    Reader r(fin);
    if (std::string e = r.open(); !e.empty()) { if (fin != stdin) std::fclose(fin); return fail(label + ": " + e); }
    (void)arc_size;

    Output w;
    if (std::string e = w.open(out, test); !e.empty()) { if (fin != stdin) std::fclose(fin); return fail(e); }
    const int hw = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    int threads = o.threads_set ? o.threads : hw;
    const std::uint64_t avail = available_memory();
    Progress progress(!o.quiet && is_tty(stderr), 0);

    std::vector<Block> blocks;
    std::vector<std::vector<std::uint8_t>> outbuf;
    std::uint32_t stream_crc = 0;
    std::uint64_t total = 0;
    std::size_t block_no = 0;
    bool end = false, ok = true, sized = false;
    std::string err;
    while (!end && ok) {
        // Read a batch: the first block decides how many threads fit in memory.
        blocks.clear();
        while (!end && blocks.size() < static_cast<std::size_t>(std::max(1, threads))) {
            Block b;
            if (std::string e = r.next(b, end); !e.empty()) { ok = false; err = e; break; }
            if (end) break;
            if (!sized) {
                sized = true;
                if (avail && !o.threads_set) {
                    const std::uint64_t per = job_memory(b.method == kCM ? b.table_bits : 16, b.slot_extra, b.raw);
                    threads = static_cast<int>(std::max<std::uint64_t>(1, std::min<std::uint64_t>(threads, avail / 4 * 3 / per)));
                }
            }
            blocks.push_back(std::move(b));
        }
        if (!ok || blocks.empty()) break;
        outbuf.resize(blocks.size());
        std::vector<std::string> errs(blocks.size());
        const bool ran = parallel_for(blocks.size(), threads, [&](std::size_t j) {
            outbuf[j].resize(blocks[j].raw);
            errs[j] = decompress_block(blocks[j], outbuf[j].data());
        });
        if (!ran) { ok = false; err = "out of memory (try fewer threads with -t)"; break; }
        for (std::size_t j = 0; j < blocks.size(); ++j, ++block_no) {
            if (!errs[j].empty()) { ok = false; err = "block " + std::to_string(block_no) + ": " + errs[j]; break; }
            stream_crc = crc32(stream_crc, outbuf[j].data(), outbuf[j].size());
            total += outbuf[j].size();
            if (!w.write(outbuf[j])) { ok = false; err = out + ": write error"; break; }
            std::vector<std::uint8_t>().swap(outbuf[j]);
        }
    }
    progress.stop();
    if (ok && r.version() == 3) {
        if (r.stream_crc() != stream_crc) { ok = false; err = "stream CRC mismatch (archive is damaged)"; }
        else if (r.trailing_data()) { ok = false; err = "unexpected data after the end of the archive"; }
    }
    if (fin != stdin) std::fclose(fin);
    if (!w.close(ok)) return fail(err.empty() ? (out == "-" ? std::string("stdout") : out) + ": write error" : label + ": " + err);
    if (!ok) return fail(label + ": " + err);
    const double s = seconds_since(t0);
    if (!o.quiet) {
        if (test)
            std::fprintf(stderr, "%s: OK, %llu B%s, %.1f s\n", label.c_str(), static_cast<unsigned long long>(total),
                         r.version() == 2 ? " (format 2: no checksums to verify)" : ", CRC-32 verified", s);
        else
            std::fprintf(stderr, "%s: %llu B, %.1f s, %.1f KiB/s\n", label.c_str(), static_cast<unsigned long long>(total), s,
                         static_cast<double>(total) / 1024.0 / (s > 0 ? s : 1));
    }
    return 0;
}

int do_list(const std::string& in) {
    std::FILE* fin = stdin;
    std::uint64_t arc_size = 0;
    if (in != "-") {
        std::error_code ec;
        arc_size = fs::file_size(to_path(in), ec);
        fin = open_file(in, false);
        if (!fin) return fail(in + ": " + std::strerror(errno));
    }
    const std::string label = in == "-" ? "stdin" : in;
    Reader r(fin);
    if (std::string e = r.open(); !e.empty()) { if (fin != stdin) std::fclose(fin); return fail(label + ": " + e); }
    std::printf("%s: hpd archive format %d\n", label.c_str(), r.version());
    std::printf("%6s %12s %12s %7s  %-6s %4s %3s %4s %5s %12s  %s\n", "block", "raw", "packed", "bpc", "method", "bits", "g", "disc",
                "x86", "passed", r.version() == 3 ? "crc32" : "");
    std::uint64_t raw = 0, packed = 0;
    bool end = false;
    std::string err;
    for (std::size_t i = 0; ; ++i) {
        Block b;
        if (std::string e = r.next(b, end, true); !e.empty()) { err = e; break; }
        if (end) break;
        const std::uint64_t len = r.payload_len();
        raw += b.raw; packed += len;
        std::printf("%6zu %12u %12llu %7.3f  %-6s %4d %3d %4d %5s %12llu  ", i, b.raw, static_cast<unsigned long long>(len),
                    b.raw ? 8.0 * static_cast<double>(len) / b.raw : 0.0, b.method == kCM ? "CM" : "stored",
                    b.method == kCM ? b.table_bits : 0, b.method == kCM ? b.slot_extra : 0, b.method == kCM ? b.disc_slots : 0,
                    (b.filters & kFilterE8) ? "yes" : "-", static_cast<unsigned long long>(b.passed));
        if (b.has_crc) std::printf("%08x", b.crc);
        std::printf("\n");
    }
    if (fin != stdin) std::fclose(fin);
    if (!err.empty()) return fail(label + ": " + err);
    std::printf("total: %llu B in, %llu B archive", static_cast<unsigned long long>(raw), static_cast<unsigned long long>(arc_size ? arc_size : packed));
    if (raw) std::printf(" (%.3f bpc)", 8.0 * static_cast<double>(arc_size ? arc_size : packed) / static_cast<double>(raw));
    if (r.version() == 3) std::printf(", CRC-32 of data %08x", r.stream_crc());
    std::printf("\n");
    return 0;
}

std::vector<std::string> utf8_args(int argc, char** argv) {
    std::vector<std::string> a;
#if defined(_WIN32)
    (void)argv;
    int n = 0;
    if (LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n)) {
        for (int i = 0; i < n; ++i) a.push_back(narrow(w[i]));
        LocalFree(w);
        return a;
    }
#endif
    for (int i = 0; i < argc; ++i) a.emplace_back(argv[i]);
    return a;
}

bool ends_with(const std::string& s, const std::string& t) {
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    binary_stdio();
    const std::vector<std::string> args = utf8_args(argc, argv);
    if (args.size() < 2) { usage(stderr); return 2; }
    const std::string mode = args[1];
    if (mode == "-h" || mode == "--help") { usage(stdout); return 0; }
    if (mode == "-V" || mode == "--version") { std::printf("%s\n", kVersionText); return 0; }
    if (mode != "c" && mode != "d" && mode != "t" && mode != "l") { usage(stderr); return 2; }

    Options o;
    std::vector<std::string> pos;
    bool only_pos = false;
    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (only_pos || a == "-" || a.empty() || a[0] != '-') { pos.push_back(a); continue; }
        if (a == "--") { only_pos = true; continue; }
        if (a == "-9") { o.s.disc_slots = 12; continue; }
        if (a == "-c" || a == "--stdout") { o.to_stdout = true; continue; }
        if (a == "-f" || a == "--force") { o.force = true; continue; }
        if (a == "-q" || a == "--quiet") { o.quiet = true; continue; }
        if (a == "-v" || a == "--verbose") { o.verbose = true; continue; }
        if (a == "-h" || a == "--help") { usage(stdout); return 0; }
        if (a == "-V" || a == "--version") { std::printf("%s\n", kVersionText); return 0; }
        // Options with a value: "-t 4" or "-t4".
        const std::string key = a.substr(0, 2);
        std::string val = a.size() > 2 ? a.substr(2) : "";
        if (val.empty()) {
            if (i + 1 >= args.size()) { std::fprintf(stderr, "hpd: %s needs a value\n", a.c_str()); return 2; }
            val = args[++i];
        }
        long long v = 0;
        bool good = true;
        if (key == "-t") { good = parse_num(val, 1, 1024, v); o.threads = static_cast<int>(v); o.threads_set = true; }
        else if (key == "-b") { good = parse_num(val, 1, kMaxBlock >> 20, v); o.block = static_cast<std::uint64_t>(v) << 20; }
        else if (key == "-B") { good = parse_num(val, 4, kMaxBlock >> 10, v); o.block = static_cast<std::uint64_t>(v) << 10; }
        else if (key == "-m") { good = parse_num(val, 16, kMaxTableBits, v); o.s.force_bits = static_cast<int>(v); }
        else if (key == "-M") { good = parse_num(val, 16, kMaxTableBits, v); o.s.bits_cap = static_cast<int>(v); }
        else if (key == "-g") { good = parse_num(val, 0, kMaxSlotExtra, v); o.s.slot_extra = static_cast<int>(v); }
        else if (key == "-D") { good = parse_num(val, 1, 12, v); o.s.disc_slots = static_cast<int>(v); }
        else if (key == "-x") { good = parse_num(val, 0, 1, v); o.s.x86_mode = static_cast<int>(v); }
        else if (key == "-s") { good = parse_num(val, 0, 1, v); o.s.bypass = v != 0; }
        else { std::fprintf(stderr, "hpd: unknown option %s\n", a.c_str()); usage(stderr); return 2; }
        if (!good) { std::fprintf(stderr, "hpd: bad value for %s: %s\n", key.c_str(), val.c_str()); return 2; }
    }

    if (mode == "t" || mode == "l") {
        if (pos.empty()) { usage(stderr); return 2; }
        int rc = 0;
        for (const std::string& f : pos) rc |= mode == "t" ? do_decompress(f, "", o, true) : do_list(f);
        return rc;
    }
    if (pos.empty() || pos.size() > 2) { usage(stderr); return 2; }
    const std::string in = pos[0];
    std::string out;
    bool derived = false;
    if (pos.size() == 2) out = pos[1];
    else if (o.to_stdout || in == "-") out = "-";
    else if (mode == "c") { out = in + ".hpd"; derived = true; }
    else if (ends_with(in, ".hpd") && in.size() > 4) { out = in.substr(0, in.size() - 4); derived = true; }
    else return fail(in + ": no .hpd suffix, give an output name or use -c");

    if (out != "-") {
        std::error_code ec;
        const fs::path po = to_path(out);
        if (in != "-" && fs::exists(po, ec) && fs::equivalent(to_path(in), po, ec)) return fail("input and output are the same file");
        if (derived && !o.force && fs::exists(po, ec)) return fail(out + " exists (use -f to overwrite)");
    } else if (mode == "c" && is_tty(stdout)) {
        return fail("compressed data not written to a terminal (redirect it, or give an output file)");
    }
    return mode == "c" ? do_compress(in, out, o) : do_decompress(in, out, o, false);
}
