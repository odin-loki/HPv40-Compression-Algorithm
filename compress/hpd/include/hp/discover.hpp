#pragma once
//
// hp/discover.hpp — automated context discovery.
//
// THE PROBLEM THIS SOLVES
// -----------------------
// fx2-cmix's context set is hand-tuned: sparse masks {1,2,4}, {2,3,4}, {7,2},
// match orders {0,8} {1,8} {7,4} {11,3} {13,2}, and so on. Those numbers were
// not derived. They are the residue of years of manual search by one person.
//
// But a search result can be reproduced by a search. This file runs that
// search ONLINE, during compression, on both sides simultaneously.
//
// HOW
// ---
// A pool of SLOTS. Each slot is a context model whose context is defined by a
// bitmask over the last 16 byte positions -- which past bytes it looks at.
// Every slot tracks its own cumulative code length.
//
// Periodically the worst-performing slot is KILLED and replaced with a fresh
// candidate mask drawn from a deterministic PRNG. Good masks survive and
// accumulate; bad ones are recycled within a few thousand bytes. This is MDL
// selection: a slot is kept only while it pays for the bits it costs.
//
// DETERMINISM
// -----------
// The PRNG is seeded identically on both sides and advanced only on events
// both sides observe. Slot performance is measured from probabilities both
// sides compute. So encoder and decoder evolve an IDENTICAL context set
// without transmitting a single bit about it. That is the whole trick -- the
// discovered model set costs nothing in S1 or S2.
//
// RESIDUAL TARGETING
// ------------------
// Slots update proportionally to how badly the ENSEMBLE erred, not to their
// own error. A slot that only fires where the ensemble is already correct
// learns almost nothing and is quickly recycled; a slot covering the
// ensemble's blind spots accumulates. This is boosting, and it produces
// decorrelation by construction rather than by penalty tuning -- avoiding the
// "learner collusion" failure mode reported for jointly-trained ensembles.

#include "hp/hd.hpp"
#include <cstdint>
#include <vector>

#include "hp/features.hpp"
#include "hp/int_math.hpp"
#include "hp/models.hpp"

namespace hp {

// Deterministic PRNG. Both sides step it identically.
class Rng {
 public:
    explicit Rng(std::uint64_t seed) : s_(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    std::uint64_t next() {
        s_ ^= s_ << 13; s_ ^= s_ >> 7; s_ ^= s_ << 17;
        return s_;
    }
 private:
    std::uint64_t s_;
};

class DiscoveryPool {
 public:
    static constexpr int kSlots = 12;            // maximum; n_ slots are live (hpd: runtime, default 4)
    static constexpr int kEvalBytes = HP_DISC_EVAL;   // review cadence
    static constexpr int kMinAge = 3;         // reviews before a slot is eligible

    DiscoveryPool(int table_bits, std::uint64_t seed, int slots = HP_DISC_SLOTS)
        : n_(slots < 1 ? 1 : (slots > kSlots ? kSlots : slots)), rng_(seed) {
        models_.reserve(static_cast<std::size_t>(n_));
        for (int i = 0; i < n_; ++i) {
            models_.emplace_back(table_bits, 255);
            mask_[i] = fresh_mask();
            loss_[i] = 0;
            age_[i] = 0;
        }
    }

    // Set each slot's context from the byte history, using its mask.
    void set_contexts(std::uint64_t hist, std::uint64_t hist2) {
        for (int i = 0; i < n_; ++i) {
            std::uint64_t key = 0;
            std::uint32_t m = mask_[i];
            int shift = 0;
            for (int b = 0; b < 16; ++b) {
                if (m & (1u << b)) {
                    const std::uint64_t byte =
                        (b < 8) ? ((hist >> (b * 8)) & 0xffull)
                                : ((hist2 >> ((b - 8) * 8)) & 0xffull);
                    key ^= byte << ((shift * 11) & 55);
                    ++shift;
                }
            }
            models_[i].set_context(hash2(0x51073ull + i, key));
        }
    }

#if HPD_BUCKET
    void prefetch_nibble(int c0) {
        for (int i = 0; i < n_; ++i) models_[i].prefetch_nibble(c0);
    }
#endif
    void predict(int c0, int backoff_p12, int* out) {
        for (int i = 0; i < n_; ++i)
            models_[i].predict(c0, backoff_p12, out + i * ContextModel::kOutputs);
        // Remember each slot's own opinion for the MDL bookkeeping.
        for (int i = 0; i < n_; ++i) p_[i] = models_[i].last_p();
    }

    // ens_err: |ensemble probability - outcome| in 12-bit. Slots are charged
    // and credited relative to how hard this bit was for the ensemble.
    void update(int y, int ens_err) {
        for (int i = 0; i < n_; ++i) {
            models_[i].update(y);
            const int pa = y ? p_[i] : 4096 - p_[i];
            const std::uint32_t cost =
                (12u << 16) - log2_q16(static_cast<std::uint32_t>(pa < 1 ? 1 : pa));
            // Weight the charge by ensemble difficulty: covering easy bits
            // earns a slot nothing.
            loss_[i] += (static_cast<std::uint64_t>(cost) *
                         static_cast<std::uint32_t>(ens_err + 64)) >> 8;
        }
    }

    // Once per byte. Runs the MDL review on schedule.
    void end_byte() {
        if (++bytes_ < kEvalBytes) return;
        bytes_ = 0;
        for (int i = 0; i < n_; ++i) ++age_[i];

        // Kill the worst eligible slot; recycle it onto a fresh candidate.
        int worst = -1;
        std::uint64_t worst_loss = 0;
        for (int i = 0; i < n_; ++i) {
            if (age_[i] < kMinAge) continue;
            if (worst < 0 || loss_[i] > worst_loss) { worst = i; worst_loss = loss_[i]; }
        }
        if (worst >= 0) {
            mask_[worst] = fresh_mask();
            age_[worst] = 0;
            ++replaced_;
        }
        // Decay all books so old evidence fades and survivors stay contestable.
        for (int i = 0; i < n_; ++i) loss_[i] = (loss_[i] * 3) >> 2;
    }

    int slots() const { return n_; }
    int outputs() const { return n_ * ContextModel::kOutputs; }
    int slot_p(int i) const { return p_[i]; }
    std::uint32_t mask(int i) const { return mask_[i]; }
    int replaced() const { return replaced_; }

 private:
    // A candidate: 2-5 byte positions out of the last 16.
    //
    // CRITICAL: candidates must be pushed AWAY from contiguous prefixes.
    // A mask like {0,1,2} is just order-3, which the fixed model set already
    // covers -- the first version of this generator produced mostly such
    // masks and the pool was net-negative because every discovered slot
    // duplicated an existing expert. Novelty is the entire point, so we
    // reject contiguous-from-zero masks and require reach beyond offset 3.
    std::uint32_t fresh_mask() {
        // B.2: one in four candidates is a skip-k template (every kth
        // position). That is a pattern class over byte *layout*, not a
        // contiguous suffix — the axis discover.hpp was missing.
        if ((rng_.next() & 3) == 0) {
            const int k = 2 + static_cast<int>(rng_.next() & 3);  // 2..5
            std::uint32_t m = 0;
            for (int i = 0; i < 16; i += k) m |= (1u << i);
            if (m != 0 && (m & 0xFFF0u) != 0) return m;
        }
        for (int attempt = 0; attempt < 8; ++attempt) {
            const std::uint64_t r = rng_.next();
            const int nbits = 2 + static_cast<int>(r & 3);
            std::uint32_t m = 0;
            std::uint64_t v = r >> 2;
            for (int k = 0; k < nbits; ++k) {
                m |= (1u << static_cast<int>(v & 15));
                v >>= 4;
            }
            if (m == 0) continue;
            // Reject: contiguous run starting at 0 (duplicates a plain order).
            const std::uint32_t contig = (m + 1) & m;
            if (contig == 0 && (m & 1)) continue;
            // Require at least one position at offset >= 4 -- the fixed set
            // already owns everything shorter.
            if ((m & 0xFFF0u) == 0) continue;
            return m;
        }
        return 0x0110u;
    }

    int n_;
    Rng rng_;
    vec<ContextModel> models_;
    std::uint32_t mask_[kSlots];
    std::uint64_t loss_[kSlots];
    int age_[kSlots];
    int p_[kSlots] = {0};
    int bytes_ = 0;
    int replaced_ = 0;
};

}  // namespace hp
