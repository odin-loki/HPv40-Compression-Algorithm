#pragma once
//
// hp/stat_gates.hpp — independent-state mixer selectors.
//
// From Stats for Compression (enwik8 measurement): a statistic pays 2–4×
// more as a mixer weight-set selector than as an extra input. Cross-family
// gates stack; same-slot ones dilute. These three are the paper's winners
// that hp does not already reconstruct:
//
//   rec_branch3  distinct successors of the order-3 byte context
//   disp_var     variance of expert opinions before mixing
//   mm_len2      second-longest standing match
//
// Integer-only. Encoder and decoder share the same bytes.

#include <cstdint>
#include <vector>

#include "hp/int_math.hpp"
#include "hp/models.hpp"

namespace hp {

class Branch3 {
 public:
    static constexpr int kBits = 18;
    Branch3() : mask_(static_cast<std::size_t>(1) << kBits, 0) {}

    // Call once per completed byte. Query uses bytes[-3:-1]; update uses
    // bytes[-4:-2] with successor = this byte. Do not record a context as
    // its own successor (cm2.cpp bug #3).
    void push_byte(int byte, std::uint64_t hist) {
        const std::uint32_t q =
            hash2(9, (hist >> 8) & 0xffffffull) & ((1u << kBits) - 1);
        int pc = 0;
        std::uint64_t m = mask_[q];
        while (m) {
            m &= m - 1;
            ++pc;
        }
        bin_ = pc > 15 ? 15 : pc;
        const std::uint32_t u =
            hash2(9, (hist >> 16) & 0xffffffull) & ((1u << kBits) - 1);
        mask_[u] |= 1ull << (static_cast<unsigned>(byte) & 63u);
    }

    int bin() const { return bin_; }

 private:
    std::vector<std::uint64_t> mask_;
    int bin_ = 0;
};

inline int shape6_bin(int n0, int n1, int p12) {
    const int n = n0 + n1;
    return ((p12 >> 9) & 7) * 2 + (n > 8 ? 1 : 0);
}

inline int disp_var_bin(const int* p12, int n) {
    if (n <= 1) return 0;
    std::int64_t sum = 0;
    for (int i = 0; i < n; ++i) sum += p12[i];
    const int mean = static_cast<int>(sum / n);
    std::int64_t var = 0;
    for (int i = 0; i < n; ++i) {
        const std::int64_t d = p12[i] - mean;
        var += d * d;
    }
    var /= n;
    return qlog_u32(static_cast<std::uint32_t>(var >> 6), 16);
}

inline int mlen2_bin(int longest, int second) {
    (void)longest;
    return qlog_u32(static_cast<std::uint32_t>(second + 1), 16);
}

inline int agree_bin(const int* p12, int n) {
    if (n <= 1) return 0;
    std::int64_t sum = 0;
    for (int i = 0; i < n; ++i) sum += p12[i] - 2048;
    const int pos = sum >= 0 ? 1 : 0;
    int ag = 0;
    for (int i = 0; i < n; ++i) {
        const int hi = p12[i] >= 2048 ? 1 : 0;
        if (hi == pos) ++ag;
    }
    return ag > 15 ? 15 : ag;
}

inline int argmax_bin(const int* p12, int n) {
    int best_i = 0, best_a = -1;
    for (int i = 0; i < n && i < 16; ++i) {
        const int a = p12[i] > 2048 ? p12[i] - 2048 : 2048 - p12[i];
        if (a > best_a) {
            best_a = a;
            best_i = i;
        }
    }
    return best_i;
}

}  // namespace hp
