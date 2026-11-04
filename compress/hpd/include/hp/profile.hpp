#pragma once
//
// hp/profile.hpp — redundancy decomposition.
//
// CTW's analysis splits cumulative redundancy into three identifiable terms:
// CODING, PARAMETER and MODEL redundancy. That decomposition is a diagnostic
// even when you are not running CTW, because it tells you WHERE your bits are
// going and therefore what is worth fixing. Without it you are guessing.
//
//   total       -- actual bits spent
//   coding      -- arithmetic coder overhead vs the ideal -log2 p. Should be
//                  near zero; if it is not, the coder or the 12-bit
//                  quantisation is broken.
//   parameter   -- bits lost because contexts are sparse and their estimates
//                  are still converging. Fix: better estimators, pooling,
//                  bigger tables. This is what indirect modelling attacks.
//   model       -- bits lost because the mixer weighted the experts badly,
//                  measured against the best single expert in hindsight.
//                  Fix: better mixing, better gates, more experts.
//
// Everything is integer, accumulated in Q16 bits, so the profiler cannot
// perturb the bit-exactness of the codec it is measuring.

#include <cstdint>
#include <cstdio>

#include "hp/int_math.hpp"
#include "hp/predictor.hpp"

namespace hp {

class Profiler {
 public:
    void account(int p_final, int y, const Predictor& pred) {
        const int pa = y ? p_final : 4096 - p_final;
        total_q16_ += cost(pa);

        // Best single expert in hindsight, this bit.
        int best = 1 << 30;
        const int n = pred.expert_count();
        for (int i = 0; i < n; ++i) {
            const int pe = pred.expert_p(i);
            const int c = cost(y ? pe : 4096 - pe);
            if (c < best) best = c;
            // Bucket by how sparse the context was, to separate parameter
            // cost (sparse, still learning) from model cost (dense, mixed
            // badly).
        }
        best_q16_ += best;

        const int mixed = pred.mixed_p();
        mix_q16_ += cost(y ? mixed : 4096 - mixed);

        // Sparse-context share: bits spent where the experts had little data.
        if (pred.sparse_fraction() > 128) sparse_q16_ += cost(pa);
        else dense_q16_ += cost(pa);

        ++bits_;
    }

    void report(std::FILE* f, std::size_t nbytes) const {
        if (!bits_) return;
        const std::uint64_t B = 1u << 16;
        std::fprintf(f, "\n--- redundancy profile (%llu bits over %zu B) ---\n",
                     (unsigned long long)bits_, nbytes);
        row(f, "total spent",   total_q16_, nbytes);
        row(f, "  best-expert", best_q16_, nbytes);
        row(f, "  after mixer", mix_q16_, nbytes);
        row(f, "  sparse ctx",  sparse_q16_, nbytes);
        row(f, "  dense ctx",   dense_q16_, nbytes);
        const std::int64_t model = static_cast<std::int64_t>(mix_q16_) -
                                   static_cast<std::int64_t>(best_q16_);
        const std::int64_t coding = static_cast<std::int64_t>(total_q16_) -
                                    static_cast<std::int64_t>(mix_q16_);
        std::fprintf(f, "  model redundancy (mixer vs best expert): %+lld B\n",
                     (long long)(model / (8 * (std::int64_t)B)));
        std::fprintf(f, "  coding redundancy (APM+coder vs mixer):  %+lld B\n",
                     (long long)(coding / (8 * (std::int64_t)B)));
        std::fprintf(f, "  parameter share (sparse contexts):       %llu%%\n",
                     (unsigned long long)(sparse_q16_ * 100 /
                        (sparse_q16_ + dense_q16_ ? sparse_q16_ + dense_q16_ : 1)));
        (void)B;
    }

 private:
    static std::uint32_t cost(int p) {
        if (p < 1) p = 1;
        return (12u << 16) - log2_q16(static_cast<std::uint32_t>(p));
    }
    static void row(std::FILE* f, const char* name, std::uint64_t q16,
                    std::size_t nbytes) {
        const std::uint64_t bytes = q16 / (8 * 65536);
        const std::uint64_t bpc_m = nbytes ? (q16 * 1000 / 65536 / nbytes) : 0;
        std::fprintf(f, "  %-14s %10llu B   %llu.%03llu bpc\n", name,
                     (unsigned long long)bytes,
                     (unsigned long long)(bpc_m / 1000),
                     (unsigned long long)(bpc_m % 1000));
    }

    std::uint64_t total_q16_ = 0, best_q16_ = 0, mix_q16_ = 0;
    std::uint64_t sparse_q16_ = 0, dense_q16_ = 0;
    std::uint64_t bits_ = 0;
};

}  // namespace hp
