#pragma once
//
// hp/mixer.hpp -- two-layer gated logistic mixer network + APM (SSE) stage.
//
// WHY TWO LAYERS
// --------------
// A single mixer with weight sets selected by one context forces a choice: a
// rich gate captures more structure but splits the training data N ways, so
// every weight vector learns from 1/N of the stream. The first build of this
// file gated a single mixer on (alpha_bucket * 256 + c0) and MEASURABLY LOST
// ~0.17% against the ungated baseline -- dilution beat the signal.
//
// The fix is the PAQ8/cmix architecture. Several layer-1 mixers each see ALL
// the inputs and ALL the data, but each is gated on a DIFFERENT context. None
// is diluted, because each one's context set is small enough to train
// densely. A layer-2 mixer then learns how much to trust each layer-1
// opinion, itself gated on the partial byte.
//
// This is what makes GRIA usable: alpha gets its own layer-1 mixer with just
// 8 weight sets (one per alpha bucket), so it trains densely, and layer 2
// learns when the entropy-regime opinion is worth listening to.
//
// Training is per-mixer against the true bit, not backpropagated. For
// logistic mixing the gradient of coding loss w.r.t. a weight is exactly
// (y - p) * input, so this is correct gradient descent at every node and
// needs no chain rule.

#include <cstdint>
#include <vector>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

#include "hp/features.hpp"
#include "hp/int_math.hpp"

namespace hp {

// Exact int32 x int32 -> int64 dot product (AVX2 when available).
inline std::int64_t dot64(const std::int32_t* w, const std::int32_t* x, int n) {
    std::int64_t sum = 0;
    int i = 0;
#if defined(__AVX2__)
    __m256i acc = _mm256_setzero_si256();
    for (; i + 8 <= n; i += 8) {
        const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + i));
        const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(x + i));
        const __m256i pe = _mm256_mul_epi32(a, b);
        const __m256i po = _mm256_mul_epi32(_mm256_srli_epi64(a, 32), _mm256_srli_epi64(b, 32));
        acc = _mm256_add_epi64(acc, _mm256_add_epi64(pe, po));
    }
    alignas(32) std::int64_t t[4];
    _mm256_store_si256(reinterpret_cast<__m256i*>(t), acc);
    sum = t[0] + t[1] + t[2] + t[3];
#endif
    for (; i < n; ++i) sum += static_cast<std::int64_t>(w[i]) * x[i];
    return sum;
}

class MixerNet {
 public:
    // n_inputs: experts. ctx_sizes: one entry per layer-1 mixer, giving how
    // many weight sets that mixer keeps. ctx2_size: layer-2 weight sets.
    MixerNet(int n_inputs, const std::vector<int>& ctx_sizes, int ctx2_size, int lr,
             const std::vector<int>& lrs = {})
        : n_(n_inputs), k_(static_cast<int>(ctx_sizes.size())), lr_(lr),
          ctx_sizes_(ctx_sizes), ctx_(ctx_sizes.size(), 0),
          st_(n_inputs, 0), dot_(ctx_sizes.size(), 0),
          pr_(ctx_sizes.size(), 2048),
          lr1_(ctx_sizes.size(), lr),
          v_(static_cast<std::size_t>(ctx2_size) * ctx_sizes.size(), 0) {
        w_.resize(ctx_sizes.size());
        const int w0 = (1 << 16) / (n_inputs > 0 ? n_inputs : 1);
        for (std::size_t j = 0; j < ctx_sizes.size(); ++j) {
            w_[j].assign(static_cast<std::size_t>(ctx_sizes[j]) * n_inputs, w0);
            if (j < lrs.size()) lr1_[j] = lrs[j];
        }
        const int v0 = (1 << 16) / (k_ > 0 ? k_ : 1);
        for (auto& x : v_) x = v0;
    }

    void reset_inputs() { m_ = 0; }
    void add(int stretched) { if (m_ < n_) st_[m_++] = stretched; }

    void set_ctx(int j, int c) { ctx_[j] = c % ctx_sizes_[j]; }
    void set_ctx2(int c) { ctx2_ = c; }

    int mix() {
        // Layer 1.
        for (int j = 0; j < k_; ++j) {
            const std::int32_t* w = &w_[j][static_cast<std::size_t>(ctx_[j]) * n_];
            const std::int64_t sum = dot64(w, st_.data(), m_);
            dot_[j] = clamp_int(static_cast<int>(sum >> 16), -2047, 2047);
            pr_[j] = squash(dot_[j]);
        }
        // Layer 2. Inputs are the layer-1 logits directly -- already in
        // stretch domain, so no table round-trip and no precision loss.
        const std::int32_t* v = &v_[static_cast<std::size_t>(ctx2_) * k_];
        std::int64_t sum = 0;
        for (int j = 0; j < k_; ++j) sum += static_cast<std::int64_t>(v[j]) * dot_[j];
        final_dot_ = clamp_int(static_cast<int>(sum >> 16), -2047, 2047);
        final_pr_ = squash(final_dot_);
        return final_pr_;
    }

    void update(int y) {
        const int t = y << 12;
        const int err2 = t - final_pr_;
        {
            const int ae = err2 < 0 ? -err2 : err2;
            if (ae < HP_MIXER_SKIP) return;
        }

        std::int32_t* v = &v_[static_cast<std::size_t>(ctx2_) * k_];
        for (int j = 0; j < k_; ++j) {
            const std::int32_t dv = static_cast<std::int32_t>(
                (static_cast<std::int64_t>(dot_[j]) * err2 * lr_) >> 14);
            v[j] = clamp_int(v[j] + dv, -(1 << 22), (1 << 22));
        }

        for (int j = 0; j < k_; ++j) {
            std::int32_t* w = &w_[j][static_cast<std::size_t>(ctx_[j]) * n_];
            const int err = t - pr_[j];
            const int l1 = lr1_[static_cast<std::size_t>(j)];
            // |st| <= 2047, |err| <= 4095, l1 <= 4: the product fits in int32,
            // so this equals the int64 form exactly and vectorises.
            const std::int32_t m = err * l1;
            const std::int32_t* x = st_.data();
            for (int i = 0; i < m_; ++i) {
                std::int32_t v = w[i] + ((x[i] * m) >> 14);
                v = v < -(1 << 22) ? -(1 << 22) : (v > (1 << 22) ? (1 << 22) : v);
                w[i] = v;
            }
        }
    }

    int num_layer1() const { return k_; }
    int layer1_p(int j) const { return pr_[static_cast<std::size_t>(j)]; }
    int layer1_dot(int j) const { return dot_[static_cast<std::size_t>(j)]; }

 private:
    int n_, k_, lr_;
    std::vector<int> ctx_sizes_;
    std::vector<int> ctx_;
    std::vector<std::int32_t> st_;
    std::vector<int> dot_, pr_;
    std::vector<int> lr1_;                       // per-mixer layer-1 rates
    std::vector<std::vector<std::int32_t>> w_;  // layer 1
    std::vector<std::int32_t> v_;               // layer 2
    int m_ = 0;
    int ctx2_ = 0;
    int final_dot_ = 0;
    int final_pr_ = 2048;
};

// ---------------------------------------------------------------------------
// APM / SSE -- adaptive probability map
// ---------------------------------------------------------------------------
//
// Learns a correction curve for a probability given a context. The mixer gets
// the ranking roughly right but is systematically mis-calibrated in specific
// contexts; the APM fixes exactly that. 33 knots evenly spaced in stretch
// domain, integer linear interpolation, both straddling knots updated so the
// curve stays smooth.

class APM {
 public:
    explicit APM(int n_ctx) : t_(static_cast<std::size_t>(n_ctx) * 33) {
        for (int i = 0; i < n_ctx; ++i) {
            for (int j = 0; j < 33; ++j) {
                t_[static_cast<std::size_t>(i) * 33 + j] =
                    static_cast<std::uint16_t>(squash((j - 16) * 128) * 16);
            }
        }
    }

    int refine(int pr, int ctx) {
        const int s = stretch(pr) + 2048;
        const int w = s & 127;
        idx_ = (s >> 7) + ctx * 33;
        return (t_[idx_] * (128 - w) + t_[idx_ + 1] * w) >> 11;
    }

    void update(int y, int rate = 7) {
        const int g = (y << 16) + (y << rate) - y - y;
        t_[idx_] = static_cast<std::uint16_t>(
            t_[idx_] + ((g - static_cast<int>(t_[idx_])) >> rate));
        t_[idx_ + 1] = static_cast<std::uint16_t>(
            t_[idx_ + 1] + ((g - static_cast<int>(t_[idx_ + 1])) >> rate));
    }

 private:
    std::vector<std::uint16_t> t_;
    int idx_ = 0;
};

}  // namespace hp
