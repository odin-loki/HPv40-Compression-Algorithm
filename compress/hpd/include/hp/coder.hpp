#pragma once
//
// hp/coder.hpp — binary arithmetic coder.
//
// This replaces Cypha's cyphalm::ArithmeticEncoder, which codes over a full
// vocabulary CDF built from a softmax in `double`. Two problems there:
//
//   1. It is not bit-reproducible across toolchains (std::exp / std::log).
//   2. It builds and scans a V-entry CDF per symbol, with a heap allocation
//      per symbol (`std::vector<double> lp_vec(...)` in predictive_codec.cpp).
//
// Binary decomposition fixes both at once: we code ONE BIT at a time, eight
// per byte. The alphabet is {0,1}, so the "CDF" is a single 16-bit number and
// the coder reduces to an interval split. No softmax, no scan, no allocation.
//
// Interval invariant: [x1, x2] with x1 <= x2, both uint32. The split point is
//   xmid = x1 + (range >> 16) * p + ((range & 0xffff) * p >> 16)
// computed entirely in 32-bit integer arithmetic, where p = P(bit = 1) in
// 16-bit. Encoder and decoder evaluate the identical expression, so they
// agree exactly by construction.

#include <cstdint>
#include <cstdio>
#include <vector>

namespace hp {

// Shared interval-split arithmetic. Defined once so the encoder and decoder
// provably cannot drift apart.
inline std::uint32_t split(std::uint32_t x1, std::uint32_t x2, int p) {
    const std::uint32_t range = x2 - x1;
    return x1 + (range >> 16) * static_cast<std::uint32_t>(p) +
           (((range & 0xffffu) * static_cast<std::uint32_t>(p)) >> 16);
}

class Encoder {
 public:
    explicit Encoder(std::FILE* out) : out_(out) {}

    // p = P(bit == 1), 16-bit (1 .. 65535). Caller must clamp.
    void encode(int bit, int p) {
        const std::uint32_t xmid = split(x1_, x2_, p);
        if (bit) x2_ = xmid; else x1_ = xmid + 1;

        // Renormalise: emit leading bytes once they agree.
        while (((x1_ ^ x2_) & 0xff000000u) == 0) {
            std::fputc(static_cast<int>(x2_ >> 24), out_);
            x1_ <<= 8;
            x2_ = (x2_ << 8) | 255u;
        }
    }

    void flush() {
        // Emit all four bytes of x1: unambiguous, costs 3 bytes over the
        // minimum. Irrelevant at enwik scale, and it removes a whole class of
        // end-of-stream edge cases.
        for (int i = 0; i < 4; ++i) {
            std::fputc(static_cast<int>(x1_ >> 24), out_);
            x1_ <<= 8;
        }
    }

 private:
    std::FILE* out_;
    std::uint32_t x1_ = 0;
    std::uint32_t x2_ = 0xffffffffu;
};

class Decoder {
 public:
    explicit Decoder(std::FILE* in) : in_(in) {
        for (int i = 0; i < 4; ++i) x_ = (x_ << 8) | next_byte();
    }

    int decode(int p) {
        const std::uint32_t xmid = split(x1_, x2_, p);
        const int bit = (x_ <= xmid) ? 1 : 0;
        if (bit) x2_ = xmid; else x1_ = xmid + 1;

        while (((x1_ ^ x2_) & 0xff000000u) == 0) {
            x1_ <<= 8;
            x2_ = (x2_ << 8) | 255u;
            x_ = (x_ << 8) | next_byte();
        }
        return bit;
    }

 private:
    std::uint32_t next_byte() {
        const int c = std::fgetc(in_);
        return c == EOF ? 0u : static_cast<std::uint32_t>(c);
    }

    std::FILE* in_;
    std::uint32_t x1_ = 0;
    std::uint32_t x2_ = 0xffffffffu;
    std::uint32_t x_ = 0;
};

}  // namespace hp
