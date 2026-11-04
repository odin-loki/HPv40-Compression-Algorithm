#pragma once
//
// hp/models.hpp — the expert set.
//
// Each model turns "what have I seen before in this context" into a
// probability for the next bit. They are deliberately simple; the point of
// step 0 is a correct, fast, bit-exact skeleton with a real ablation harness,
// not a maximal model zoo. Adding models is the step-2/3 work and it bolts on
// here without touching the coder or the mixer.

#include "hp/hd.hpp"
#include <cstdint>
#include <cstring>
#include <vector>

#include "hp/features.hpp"
#include "hp/int_math.hpp"
#include "hp/statemap.hpp"
#include "hp/zero_array.hpp"

namespace hp {

// ---------------------------------------------------------------------------
// Integer hashing. Splitmix64 finaliser -- fast, well-distributed, exact.
// ---------------------------------------------------------------------------
inline std::uint64_t mix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

inline std::uint32_t hash2(std::uint64_t a, std::uint64_t b) {
    return static_cast<std::uint32_t>(mix64(a * 0x100000001B3ull + b));
}

// ---------------------------------------------------------------------------
// Adaptive bit counter
// ---------------------------------------------------------------------------
//
// p is P(bit == 1) in 16-bit. The update is a running mean that decays into a
// fixed-rate EMA once the observation count hits `limit`:
//
//     p += (target - p) / (n + 2)
//
// Integer division truncates toward zero -- deterministic, and the small bias
// it introduces is symmetric and harmless. Low-order contexts get a high
// limit (they are near-stationary and want a long memory); high-order
// contexts get a low limit so they can track local structure.

struct Counter {
    std::uint16_t p;
    std::uint16_t n;
};

inline void counter_init(Counter* c, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) { c[i].p = 32768; c[i].n = 0; }
}

inline int counter_predict_p(const Counter& c) { return c.p >> 4; }

inline int counter_predict(const Counter& c) {
    return stretch(c.p >> 4);
}

inline void counter_update(Counter& c, int y, int limit) {
    const int target = y ? 65535 : 0;
    const int d = target - static_cast<int>(c.p);
    c.p = static_cast<std::uint16_t>(static_cast<int>(c.p) + d / (c.n + 2));
    if (c.n < limit) ++c.n;
}

// ---------------------------------------------------------------------------
// A single hashed context model
// ---------------------------------------------------------------------------
#ifndef HPD_BUCKET
#define HPD_BUCKET 1
#endif

#if HPD_BUCKET
// Bucketed context model (hpd). One 32-byte bucket per (context, nibble):
// a 16-bit checksum plus 15 bit-history slots, one per node of the nibble's
// binary tree. Two buckets share a 64-byte cache line and form a 2-way set.
// The bucket is chosen once per nibble, so a model touches 2 cache lines per
// byte instead of 8 random slots, and the line can be prefetched one call
// ahead (set_context / prefetch_nibble) so lookups across models overlap.
// The checksum stops unrelated contexts sharing statistics (HP40 had none).
class ContextModel {
 public:
    static constexpr int kOutputs = 2;
    struct Bucket { std::uint16_t chk; std::uint16_t s[15]; };
    static_assert(sizeof(Bucket) == 32, "bucket must be 32 bytes");

    // Same bytes as the flat table it replaces: 2 B x 2^table_bits.
    ContextModel(int table_bits, int limit)
        : nb_(std::size_t(1) << (table_bits > 8 ? table_bits - 4 : 4)),
          limit_(limit), t_(nb_), sm_(StateTable::kStates) {}
    ContextModel(ContextModel&&) noexcept = default;

    void set_context(std::uint32_t h) {
        h_ = h; idle_ = false; cur_ = nullptr; nib_ = 0;
        locate(0);
    }
    void set_idle() { idle_ = true; h_ = 0; cur_ = nullptr; nib_ = 0; locate(0); }

    // Called for every model right after bit 3 is coded, before any model
    // predicts bit 4, so the second-nibble lines load in parallel.
    void prefetch_nibble(int c0) { cur_ = nullptr; nib_ = 1; locate(16 | (c0 & 15)); }

    void predict(int c0, int backoff_p12, int* out) {
        if (c0 >= 16 && !nib_) prefetch_nibble(c0);
        if (!cur_) select();
        int node;
        if (c0 < 16) node = c0;                                   // bits 0..3: c0 = 1..15
        else {
            const int bp = c0 < 32 ? 4 : c0 < 64 ? 5 : c0 < 128 ? 6 : 7;
            const int k = bp - 4;
            node = (1 << k) | (c0 & ((1 << k) - 1));
        }
        slot_ = &cur_->s[node - 1];
        state_ = *slot_;
        p_ind_ = sm_.predict(state_);
        const StateTable& st = state_table();
        {
            int a0 = st.n0(state_), a1 = st.n1(state_);
            if (a0 > 12) a0 = 12;
            if (a1 > 12) a1 = 12;
            p_py_ = py_estimate(a0, a1, backoff_p12);
        }
        out[0] = stretch(p_ind_);
        out[1] = stretch(p_py_);
    }

    int last_p() const { return p_ind_; }
    int last_py() const { return p_py_; }
    int n0() const { return state_table().n0(state_); }
    int n1() const { return state_table().n1(state_); }
    int sparsity() const {
        const StateTable& st = state_table();
        const int n = st.n0(state_) + st.n1(state_);
        return n >= 8 ? 0 : 255 - n * 32;
    }

    void update(int y, int ens_p12 = -1) {
        if (idle_) return;
        std::int32_t ncl = 0;
        if (ens_p12 >= 0) {
            const std::int32_t diff =
                (static_cast<std::int32_t>(p_ind_) -
                 static_cast<std::int32_t>(ens_p12)) << 10;
            ncl = (diff * HP_NCL_LAMBDA) >> 8;
        }
        sm_.update(y, limit_, ncl);
        *slot_ = static_cast<std::uint16_t>(state_table().next(state_, y));
    }

 private:
    static std::uint64_t mix64(std::uint64_t x) {
        x ^= x >> 33; x *= 0xff51afd7ed558ccdull;
        x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull;
        x ^= x >> 33; return x;
    }
    void locate(int nib) {
        const std::uint64_t k = mix64((static_cast<std::uint64_t>(h_) << 8) ^ static_cast<std::uint64_t>(nib)
                                      ^ 0x9E3779B97F4A7C15ull);
        bidx_ = static_cast<std::size_t>(k >> 24) & (nb_ - 1) & ~std::size_t(1);   // even = line start
        chk_ = static_cast<std::uint16_t>(k | 1u);                              // never 0
#if defined(__GNUC__) || defined(__clang__)
#if !defined(__CUDA_ARCH__)
        __builtin_prefetch(&t_[bidx_], 1, 3);
#endif
#endif
    }
    void select() {
        Bucket* a = &t_[bidx_];
        Bucket* b = &t_[bidx_ + 1];
        if (a->chk == chk_) { cur_ = a; return; }
        if (b->chk == chk_) { cur_ = b; return; }
        if (idle_) { cur_ = &scratch_; scratch_ = Bucket{}; return; }   // never pollute when idle
        // Replace the bucket whose first-bit history has seen fewer bits.
        const StateTable& st = state_table();
        const int pa = a->chk ? st.n0(a->s[0]) + st.n1(a->s[0]) : -1;
        const int pb = b->chk ? st.n0(b->s[0]) + st.n1(b->s[0]) : -1;
        Bucket* v = pa <= pb ? a : b;
        std::memset(v, 0, sizeof(Bucket));
        v->chk = chk_;
        cur_ = v;
    }

    std::size_t nb_;
    int limit_;
    ZeroArray<Bucket> t_;
    StateMap sm_;
    std::uint32_t h_ = 0;
    bool idle_ = false;
    std::size_t bidx_ = 0;
    int nib_ = 0;
    std::uint16_t chk_ = 1;
    Bucket* cur_ = nullptr;
    Bucket scratch_{};
    std::uint16_t* slot_ = nullptr;
    int state_ = 0;
    int p_ind_ = 2048;
    int p_py_ = 2048;
};
#else
class ContextModel {
 public:
    // Two outputs per bit now:
    //   [0] indirect  -- StateMap(bit history)   : pooled across contexts
    //   [1] direct PY -- Pitman-Yor discounted   : this context's own counts
    // Both come from the SAME stored state, so cost is 2 bytes per slot instead
    // of the 4 the old {p,n} counter used -- twice the table for the same RAM,
    // which is itself worth a couple of percent.
    static constexpr int kOutputs = 2;

    ContextModel(int table_bits, int limit)
        : mask_((1u << table_bits) - 1), limit_(limit),
          t_(static_cast<std::size_t>(1) << table_bits, 0),
          sm_(StateTable::kStates) {}

    void set_context(std::uint32_t h) { h_ = h; idle_ = false; }
    // fx2 sets(): keep a mixer slot but do not pollute the table.
    void set_idle() { idle_ = true; h_ = 0; }

    // Writes kOutputs stretched values into out[]. backoff is the parent
    // order's probability, used by the PY estimate.
    void predict(int c0, int backoff_p12, int* out) {
        idx_ = (h_ ^ (static_cast<std::uint32_t>(c0) * 0x9E3779B1u)) & mask_;
        state_ = t_[idx_];
        p_ind_ = sm_.predict(state_);
        const StateTable& st = state_table();
        {
            int a0 = st.n0(state_), a1 = st.n1(state_);
            if (a0 > 12) a0 = 12;
            if (a1 > 12) a1 = 12;
            p_py_ = py_estimate(a0, a1, backoff_p12);
        }
        out[0] = stretch(p_ind_);
        out[1] = stretch(p_py_);
    }

    int last_p() const { return p_ind_; }
    int last_py() const { return p_py_; }
    int n0() const { return state_table().n0(state_); }
    int n1() const { return state_table().n1(state_); }

    // 0..255 sparsity signal: high when this context has little history.
    int sparsity() const {
        const StateTable& st = state_table();
        const int n = st.n0(state_) + st.n1(state_);
        return n >= 8 ? 0 : 255 - n * 32;
    }

    void update(int y, int ens_p12 = -1) {
        if (idle_) return;
        std::int32_t ncl = 0;
        if (ens_p12 >= 0) {
            const std::int32_t diff =
                (static_cast<std::int32_t>(p_ind_) -
                 static_cast<std::int32_t>(ens_p12)) << 10;
            ncl = (diff * HP_NCL_LAMBDA) >> 8;
        }
        sm_.update(y, limit_, ncl);
        t_[idx_] = static_cast<std::uint16_t>(state_table().next(state_, y));
    }

 private:
    std::uint32_t mask_;
    int limit_;
    vec<std::uint16_t> t_;  // bit-history states (882 states -> 16 bit)
    StateMap sm_;
    std::uint32_t h_ = 0;
    bool idle_ = false;
    std::uint32_t idx_ = 0;
    int state_ = 0;
    int p_ind_ = 2048;
    int p_py_ = 2048;
};
#endif

// ---------------------------------------------------------------------------
// Match model
// ---------------------------------------------------------------------------
//
// Finds the most recent occurrence of the current MINLEN-byte suffix and
// predicts that the next byte repeats. On text this is the single most
// valuable model after low-order contexts: it captures repeated phrases,
// boilerplate, and -- on enwik -- repeated markup structures, at unbounded
// distance.
//
// Confidence is not hard-coded. A small counter table indexed by
// (quantised match length, predicted bit) learns how much the match model
// deserves to be trusted at each length. That keeps the model honest when it
// is wrong and lets it be very sharp when it is reliably right.

class MatchModel {
 public:
    static constexpr int kMinLen = 6;

    // order = how many past bytes are hashed to find a match. A BANK of
    // these at different orders is the cheapest real diversity available:
    // short orders fire often and loosely, long orders fire rarely and
    // authoritatively, and they decorrelate from each other far more than
    // adjacent context models do.
    MatchModel(int buf_bits, int table_bits, int order = 6,
               shared<HistoryBuf> hist = nullptr)
        : order_(order), buf_bits_(buf_bits),
          hist_(hist ? std::move(hist) : make_shared_hd<HistoryBuf>(buf_bits)),
          buf_mask_(hist_->mask),
          tab_mask_((1u << table_bits) - 1),
          buf_(hist_->a.data()),
          tab_(static_cast<std::size_t>(1) << table_bits),
          st_(64) {
        counter_init(st_.data(), st_.size());
    }

    // Called once per byte, after `byte` has been appended to `hist`
    // (so the low 6 bytes of hist are the current kMinLen-byte suffix).
    void push_byte(int byte, std::uint64_t hist) {
        // 1. Verify the standing prediction before anything else.
        if (len_ > 0) {
            if (ptr_ < pos_ && buf_[ptr_ & buf_mask_] == static_cast<std::uint8_t>(byte)) {
                if (len_ < 65535) ++len_;
                ++ptr_;
            } else {
                len_ = 0;
            }
        }

        // 2. Append.
        buf_[pos_ & buf_mask_] = static_cast<std::uint8_t>(byte);
        ++pos_;

        // 3. Index this suffix; adopt a new match if we have none.
        const std::uint64_t maskbits =
            (order_ >= 8) ? ~0ull : ((1ull << (order_ * 8)) - 1ull);
        const std::uint32_t h =
            hash2(0x4D415443ull + static_cast<std::uint64_t>(order_),
                  hist & maskbits) & tab_mask_;
        if (len_ == 0) {
            const std::uint32_t cand = tab_[h];
            if (cand > 0 && cand < pos_) {
                ptr_ = cand;
                len_ = 1;
            }
        }
        tab_[h] = pos_;

        // 4. Drop the match if it has fallen out of the ring buffer.
        if (len_ > 0 && (pos_ - ptr_) > buf_mask_) len_ = 0;
    }

    // Once per bit. bitpos is 0..7, c0 is the partial byte with sentinel.
    int predict(int c0, int bitpos) {
        valid_ = false;
        if (len_ == 0 || ptr_ >= pos_) return 0;

        const int pred_byte = buf_[ptr_ & buf_mask_];
        // The match only stands if the bits decoded so far in the current
        // byte agree with the predicted byte.
        if (bitpos > 0) {
            if (((pred_byte | 0x100) >> (8 - bitpos)) != c0) {
                len_ = 0;
                return 0;
            }
        }
        expected_ = (pred_byte >> (7 - bitpos)) & 1;
        const int lq = len_ > 31 ? 31 : len_;  // quantised length
        sidx_ = lq * 2 + expected_;
        valid_ = true;
        return counter_predict(st_[sidx_]);
    }

    void update(int y) {
        if (valid_) counter_update(st_[sidx_], y, 255);
    }

    int match_len() const { return len_; }

 private:
    int order_;
    int buf_bits_;
    shared<HistoryBuf> hist_;   // must precede buf_mask_ / buf_
    std::uint32_t buf_mask_;
    std::uint32_t tab_mask_;
    std::uint8_t* buf_;
    ZeroArray<std::uint32_t> tab_;
    vec<Counter> st_;
    std::uint32_t pos_ = 0;
    std::uint32_t ptr_ = 0;
    int len_ = 0;
    int expected_ = 0;
    int sidx_ = 0;
    bool valid_ = false;
};


// ---------------------------------------------------------------------------
// Hebbian associative word model
// ---------------------------------------------------------------------------
//
// "Cells that fire together wire together." When word A is followed by word
// B, the association A->B is POTENTIATED. All associations DECAY slowly
// (synaptic scaling), so stale links fade without ever being explicitly
// deleted. The winner for a given A is whatever currently has the strongest
// synapse.
//
// WHY THIS IS THE REVERSE OF THE DICTIONARY THAT FAILED
// ----------------------------------------------------
// The static dictionary built a word table from the input and SHIPPED it:
// the transform won 1,498 B and the 10,121 B of stored table destroyed the
// gain. A Hebbian dictionary costs ZERO bytes to transmit, because encoder
// and decoder potentiate the identical synapses from data both already have.
// It is a dictionary that is learned rather than stored -- which removes the
// exact term that killed the previous attempt.
//
// It also fires on a different axis from every context model: association
// strength is not a suffix statistic, so it should raise effective rank.

class HebbianModel {
 public:
    HebbianModel(int table_bits, int limit)
        : mask_((1u << table_bits) - 1),
          syn_(static_cast<std::size_t>(1) << table_bits),
          sm_(StateTable::kStates),
          t_(static_cast<std::size_t>(1) << table_bits),
          limit_(limit) {}

    // Called at each word boundary with the completed word and its
    // predecessor. Potentiates prev -> cur.
    void potentiate(std::uint64_t prev_word, std::uint64_t cur_word) {
        if (prev_word == 0) return;
        const std::uint32_t slot = static_cast<std::uint32_t>(
            mix64(prev_word)) & mask_;
        Syn& s = syn_[slot];
        if (s.target == cur_word) {
            if (s.strength < 255) ++s.strength;          // potentiation
        } else if (s.strength > 0) {
            --s.strength;                                 // competition
            if (s.strength == 0) s.target = cur_word;     // takeover
        } else {
            s.target = cur_word;
            s.strength = 1;
        }
        // Synaptic scaling: global slow decay keeps strengths bounded and
        // lets the network forget associations that stop being reinforced.
        if ((++tick_ & 0x3FF) == 0 && s.strength > 0) --s.strength;
    }

    // Context for the current bit: the strongest association from the
    // previous word, bucketed by synaptic strength.
    void set_context(std::uint64_t prev_word) {
        const std::uint32_t slot = static_cast<std::uint32_t>(
            mix64(prev_word)) & mask_;
        const Syn& s = syn_[slot];
        strength_ = s.strength;
        h_ = hash2(0x48454242ull,
                   s.target * 131ull + static_cast<std::uint64_t>(
                       s.strength >= 32 ? 3 : (s.strength >= 8 ? 2 :
                       (s.strength >= 2 ? 1 : 0))));
    }

    int predict(int c0) {
        idx_ = (h_ ^ (static_cast<std::uint32_t>(c0) * 0x9E3779B1u)) & mask_;
        state_ = t_[idx_];
        return stretch(sm_.predict(state_));
    }

    void update(int y) {
        sm_.update(y, limit_);
        t_[idx_] = static_cast<std::uint16_t>(state_table().next(state_, y));
    }

    int strength() const { return strength_; }

 private:
    struct Syn { std::uint64_t target = 0; std::uint16_t strength = 0; };
    std::uint32_t mask_;
    ZeroArray<Syn> syn_;
    StateMap sm_;
    ZeroArray<std::uint16_t> t_;
    int limit_;
    std::uint32_t h_ = 0, idx_ = 0;
    int state_ = 0, strength_ = 0;
    std::uint32_t tick_ = 0;
};

}  // namespace hp
