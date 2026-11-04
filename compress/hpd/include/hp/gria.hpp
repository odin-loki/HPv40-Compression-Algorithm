#pragma once
//
// hp/gria.hpp — GRIA order parameter, integer-only, as a mixer gating context.
//
// GRIA defines            alpha = 1 - H(f(X)) / H(X)
//
// In a compressor this instantiates directly and without hand-waving:
//
//   H(X)     = order-0 entropy of the source over a sliding window, in
//              bits/byte. Computed exactly from a 256-bin integer histogram.
//   H(f(X))  = the code length the compressor actually spent on that same
//              window, in bits/byte. Known exactly -- it is the sum of
//              -log2(p) over the coded bits.
//
// So alpha is the fraction of the source's order-0 entropy that the model has
// destroyed. alpha = 0 means we are doing no better than a static order-0
// coder; alpha -> 1 means we have modelled the window almost perfectly.
//
// WHY THIS IS A GATE AND NOT A CODER
// ----------------------------------
// Code length is exactly SUM -log2 P(x_t | x_<t) and nothing else. A windowed
// scalar cannot produce that sum, so alpha can never itself compress anything.
// What it CAN do is tell the mixer which regime it is in. enwik9 alternates
// hard between XML markup, running prose, link tables, and base64-ish blobs.
// Those regimes want genuinely different expert weightings: in markup the
// high-order and match models dominate; in prose the word model earns its
// keep. A mixer gated on entropy regime learns separate weight vectors for
// each, instead of one compromise vector smeared across all of them.
//
// This is the same slot PAQ fills with hand-picked contexts (match state, "am
// I inside a tag"). The claim under test is that a principled information-
// theoretic order parameter is a better gate than hand-picked heuristics.
// The --no-gria flag exists to answer that question, not to decorate it.
//
// DETERMINISM
// -----------
// Every quantity below is an integer. The entropy sums use log2_q16, which is
// exact integer normalise-and-square. The window is a fixed-size ring updated
// identically by encoder and decoder after each byte is known, so both sides
// compute the same bucket at the same bit. No synchronisation channel is
// needed and none is transmitted -- alpha is derived purely from data the
// decoder already has.

#include <cstdint>
#include <cstring>

#include "hp/int_math.hpp"

namespace hp {

class GriaGate {
 public:
    static constexpr int kWindow = 4096;   // bytes in the sliding window
    static constexpr int kRefresh = 256;   // recompute alpha every N bytes
    static constexpr int kBuckets = 24;    // alpha(8) x trajectory(3)
    static constexpr int kEntBuckets = 16; // source-entropy levels, half-bit steps

    // MEASURED CALIBRATION. Quantising alpha uniformly over [0,1] wastes the
    // scale: on wiki-like data 96% of bytes land in two buckets, because
    // alpha is a RATIO and both of its terms move together across regimes
    // (markup has low source entropy AND low achieved cost). Rescaling to the
    // observed dynamic range [0.25, 0.90] spreads the mass out. This is why
    // the raw ratio alone is a weak gate and the components must be used too.
    static constexpr int kAlphaLo = 16384;  // 0.25 in Q16
    static constexpr int kAlphaHi = 58982;  // 0.90 in Q16

    GriaGate() {
        std::memset(hist_, 0, sizeof(hist_));
        std::memset(ring_, 0, sizeof(ring_));
        for (int i = 0; i < kWindow; ++i) cost_[i] = 0;
    }

    // Enable/disable. When disabled, bucket() is pinned to 0 and the mixer
    // sees a single alpha class -- the ablation baseline.
    void set_enabled(bool on) { enabled_ = on; }

    // Composite GRIA gate: alpha level x alpha trajectory.
    //   level      -- how ordered is this window (rescaled, 8 steps)
    //   trajectory -- is order rising, flat, or falling (3 states).
    // The trajectory term is the delta_alpha idea from Cypha's existing
    // gria_gated_blend_logit: a regime CHANGE is more informative than a
    // regime level, because that is exactly when the old weights go stale.
    int bucket() const { return enabled_ ? (alpha_level_ * 3 + traj_) : 0; }

    // Source-entropy gate: H(X) alone, in half-bit steps, 0..15. Unlike
    // alpha this does NOT normalise away regime differences -- base64 blobs
    // sit near 6-8 bits/byte, markup near 4, prose near 4.5. Available as a
    // gate in its own right.
    int entropy_bucket() const { return enabled_ ? ent_bucket_ : 0; }

    int alpha_q16() const { return alpha_q16_; }

    // SWITCHING RATE for the fixed-share mixer, in Q16.
    // This is alpha used as a STATISTIC rather than as an operator: the
    // magnitude of its recent change measures how fast the source regime is
    // moving, and that is exactly the rate at which a tracking algorithm
    // should leak weight back to uniform. Stable regime -> low sigma, weights
    // concentrate on the locally best experts. Regime break -> high sigma,
    // the ensemble re-opens.
    //   floor 1/1024, ceiling 1/16 of total mass per step.
    std::uint32_t switch_rate_q16() const {
        if (!enabled_) return 64;               // fixed baseline when ablated
        std::uint32_t s = 64 + (abs_delta_ >> 3);
        return s > 4096 ? 4096u : s;
    }

    // Accumulate the cost of one coded bit. p_actual is the 12-bit
    // probability that the model assigned to the bit that actually occurred.
    // -log2(p/4096) = 12 - log2(p), in Q16.
    void account_bit(int p_actual) {
        if (p_actual < 1) p_actual = 1;
        pending_cost_ += (12u << 16) - log2_q16(static_cast<std::uint32_t>(p_actual));
    }

    // Call once per byte, after the byte is known on both sides.
    void account_byte(int byte) {
        // Evict the byte leaving the window.
        const int old = ring_[pos_];
        if (filled_) {
            if (hist_[old] > 0) --hist_[old];
            cost_sum_ -= cost_[pos_];
        }
        // Admit the new byte.
        ring_[pos_] = static_cast<std::uint8_t>(byte);
        cost_[pos_] = pending_cost_;
        cost_sum_ += pending_cost_;
        ++hist_[byte];
        pending_cost_ = 0;

        pos_ = (pos_ + 1) & (kWindow - 1);
        if (pos_ == 0) filled_ = true;
        if (!filled_) ++n_;

        if (++since_refresh_ >= kRefresh) {
            since_refresh_ = 0;
            recompute();
        }
    }

 private:
    void recompute() {
        const std::uint32_t n = filled_ ? static_cast<std::uint32_t>(kWindow)
                                        : static_cast<std::uint32_t>(n_);
        if (n < 64) return;

        // H(X) = log2(n) - (1/n) * SUM c_i * log2(c_i)   [Q16 bits/byte]
        std::uint64_t acc = 0;
        for (int i = 0; i < 256; ++i) {
            if (hist_[i] > 1) acc += nlog2n_q16(hist_[i]);
        }
        const std::uint64_t h_src = static_cast<std::uint64_t>(log2_q16(n)) - (acc / n);

        // H(f(X)) = achieved code length per byte [Q16 bits/byte]
        const std::uint64_t h_code = cost_sum_ / n;

        if (h_src == 0) return;

        // alpha = 1 - h_code / h_src
        std::int64_t a = 65536 - static_cast<std::int64_t>((h_code << 16) / h_src);
        if (a < 0) a = 0;
        if (a > 65535) a = 65535;
        const int prev = alpha_q16_;
        alpha_q16_ = static_cast<int>(a);

        // Level: rescale the observed dynamic range across 8 steps.
        std::int64_t lvl = (static_cast<std::int64_t>(alpha_q16_) - kAlphaLo) * 8 /
                           (kAlphaHi - kAlphaLo);
        alpha_level_ = static_cast<int>(lvl < 0 ? 0 : (lvl > 7 ? 7 : lvl));

        // Trajectory: falling / flat / rising. The dead band keeps the gate
        // from thrashing on sampling noise.
        const int d = alpha_q16_ - prev;
        traj_ = (d < -768) ? 0 : ((d > 768) ? 2 : 1);
        // Smoothed |delta alpha| -- the regime-change magnitude.
        const std::uint32_t ad = static_cast<std::uint32_t>(d < 0 ? -d : d);
        abs_delta_ = (abs_delta_ * 3 + ad) >> 2;

        // Source entropy in half-bit steps: h_src is Q16 bits/byte.
        int eb = static_cast<int>(h_src >> 15);
        ent_bucket_ = eb < 0 ? 0 : (eb > kEntBuckets - 1 ? kEntBuckets - 1 : eb);
    }

    bool enabled_ = true;
    std::uint8_t ring_[kWindow];
    std::uint32_t cost_[kWindow];
    std::uint32_t hist_[256];
    std::uint64_t cost_sum_ = 0;
    std::uint32_t pending_cost_ = 0;
    int pos_ = 0;
    int n_ = 0;
    bool filled_ = false;
    int since_refresh_ = 0;
    int alpha_level_ = 0;
    int traj_ = 1;
    int ent_bucket_ = 0;
    int alpha_q16_ = 0;
    std::uint32_t abs_delta_ = 0;
};

}  // namespace hp
