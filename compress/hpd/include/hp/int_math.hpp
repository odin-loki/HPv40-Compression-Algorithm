#pragma once
//
// hp/int_math.hpp — integer-only numeric primitives.
//
// HARD RULE FOR THIS DIRECTORY: no `float`, no `double`, no <cmath>, anywhere
// in the coding path, including table construction. The Hutter Prize verifies
// by running YOUR decompressor on THEIR machine. A one-ULP difference in a
// libm exp() changes one probability, changes one arithmetic-coder interval,
// and the decoder desynchronises catastrophically. Every table below is built
// by exact integer arithmetic, so every conforming C++ compiler on every
// platform produces bit-identical output.
//
// test/no_float_check.sh enforces this mechanically.

#include <cstdint>
#include <cstddef>

namespace hp {

// ---------------------------------------------------------------------------
// squash / stretch
// ---------------------------------------------------------------------------
//
// The logistic pair used throughout the PAQ lineage.
//
//   squash(x) = 4096 / (1 + e^(-x/256))     domain [-2047, 2047] -> [0, 4095]
//   stretch   = squash^-1                   domain [0, 4095] -> [-2047, 2047]
//
// Probabilities live in 12-bit "probability domain" (0..4095). Mixing happens
// in "stretch domain" (logit space, -2047..2047), because a linear mix of
// logits is the correct way to combine independent expert opinions.
//
// squash is a hard-coded 33-point table with integer linear interpolation --
// the classic PAQ formulation. No transcendental is ever evaluated.

inline int squash(int d) {
    static const int t[33] = {
        1,    2,    3,    6,    10,   16,   27,   45,   73,   120,  194,
        310,  488,  747,  1101, 1546, 2047, 2549, 2994, 3348, 3607, 3785,
        3901, 3975, 4024, 4050, 4068, 4079, 4085, 4089, 4092, 4093, 4094
    };
    if (d > 2047) return 4095;
    if (d < -2047) return 0;
    const int w = d & 127;
    const int i = (d >> 7) + 16;
    return (t[i] * (128 - w) + t[i + 1] * w + 64) >> 7;
}

// Stretch is built once by inverting squash exactly. Deterministic by
// construction: it only ever reads squash().
class StretchTable {
 public:
    StretchTable() {
        int pi = 0;
        for (int x = -2047; x <= 2047; ++x) {
            const int v = squash(x);
            for (int j = pi; j <= v; ++j) t_[j] = static_cast<std::int16_t>(x);
            pi = v + 1;
        }
        for (int j = pi; j < 4096; ++j) t_[j] = 2047;
    }
    int operator()(int p) const { return t_[p]; }

 private:
    std::int16_t t_[4096];
};

inline const StretchTable& stretch_table() {
    static const StretchTable t;
    return t;
}

inline int stretch(int p) { return stretch_table()(p); }

// ---------------------------------------------------------------------------
// fixed-point log2, Q16
// ---------------------------------------------------------------------------
//
// Needed by the GRIA alpha estimator (entropy ratios). Computed by integer
// normalise-and-square: the integer part is the bit index, and each fractional
// bit falls out of one squaring step. Exact and identical everywhere.
//
// Returns log2(x) scaled by 65536. x must be > 0.

inline std::uint32_t log2_q16_exact(std::uint32_t x) {
    if (x == 0) return 0;

    // Integer part: index of the highest set bit.
    int e = 0;
    {
        std::uint32_t v = x;
        if (v >= (1u << 16)) { v >>= 16; e += 16; }
        if (v >= (1u << 8))  { v >>= 8;  e += 8;  }
        if (v >= (1u << 4))  { v >>= 4;  e += 4;  }
        if (v >= (1u << 2))  { v >>= 2;  e += 2;  }
        if (v >= (1u << 1))  {           e += 1;  }
    }

    // Normalise mantissa to Q31 in [2^31, 2^32) i.e. value in [1, 2).
    std::uint64_t m = static_cast<std::uint64_t>(x) << (31 - e);

    // Extract 16 fractional bits. m*m < 2^64 always, so no overflow.
    std::uint32_t frac = 0;
    for (int i = 0; i < 16; ++i) {
        m = (m * m) >> 31;
        if (m >= (1ull << 32)) {
            frac |= (1u << (15 - i));
            m >>= 1;
        }
    }
    return (static_cast<std::uint32_t>(e) << 16) | frac;
}

// hpd: almost every call is a 12-bit probability (1..4096). Cache those
// results; values are produced by log2_q16_exact, so output is bit-identical.
struct Log2Table {
    std::uint32_t v[4097];
    Log2Table() { for (std::uint32_t x = 0; x <= 4096; ++x) v[x] = log2_q16_exact(x); }
};
inline const Log2Table& log2_table() { static const Log2Table t; return t; }
inline std::uint32_t log2_q16(std::uint32_t x) {
    return x <= 4096 ? log2_table().v[x] : log2_q16_exact(x);
}

// n * log2(n) in Q16, saturating into uint64. Used for entropy sums.
inline std::uint64_t nlog2n_q16(std::uint32_t n) {
    if (n <= 1) return 0;
    return static_cast<std::uint64_t>(n) * log2_q16(n);
}

// ---------------------------------------------------------------------------
// adaptive counter rate
// ---------------------------------------------------------------------------
//
// Divisor table for counter updates: p += (target - p) / rate(n).
// Early observations move fast, later ones settle -- this is the integer
// equivalent of a 1/n running mean converging to a fixed-rate EMA.

inline int clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Floor log2 into [0, nb). Used to quantise counts for mixer gates
// (Stats for Compression / cm2.cpp qlog).
inline int qlog_u32(std::uint32_t v, int nb) {
    int r = 0;
    while (v > 1 && r < nb - 1) {
        v >>= 1;
        ++r;
    }
    return r;
}

}  // namespace hp
