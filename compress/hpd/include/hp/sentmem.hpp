#pragma once
//
// hp/sentmem.hpp — slim similar-sentence memory (fx2 SimilarSentence, cut down).
//
// Ring of completed sentences. Each slot keeps a rolling hash plus the first
// kWords word hashes. The current sentence is scored by exact word-hash
// overlap; a hit (>= 53%) exposes the matched sentence hash as a context.
//
// HP_SENT_DOM=1 keeps four independent rings (prose / list / table / link),
// matching fxcm_v26's grouped sentence memory.

#include <cstdint>

#include "hp/models.hpp"


namespace hp {

class SentenceMemory {
 public:
    static constexpr int kRing = HP_SENT_MEM_BIG ? 128 : 64;
    static constexpr int kWords = HP_SENT_MEM_BIG ? 32 : 16;
    static constexpr int kDom = HP_SENT_DOM ? 4 : 1;

    void set_domain(int d) {
        if (d < 0) d = 0;
        if (d >= kDom) d = kDom - 1;
        if (d != dom_) {
            end_sentence();
            dom_ = d;
        }
    }

    void push_word(std::uint64_t wh) {
        if (wh == 0) return;
        if (cur_n_ < kWords) cur_w_[cur_n_] = wh;
        if (cur_n_ < 255) ++cur_n_;
        cur_h_ = mix64(cur_h_ * 0x100000001B3ull + wh);
        rematch();
    }

    void end_sentence() {
        if (cur_n_ > 0) {
            Slot& s = ring_[dom_][idx_[dom_]];
            s.h = cur_h_;
            s.n = cur_n_ > kWords ? kWords : cur_n_;
            for (int i = 0; i < s.n; ++i) s.w[i] = cur_w_[i];
            idx_[dom_] = (idx_[dom_] + 1) & (kRing - 1);
        }
        cur_n_ = 0;
        cur_h_ = 0;
        match_h_ = 0;
        match_score_ = 0;
        align_w_ = 0;
    }

    std::uint64_t match_hash() const { return match_h_; }
    std::uint64_t aligned_word() const { return align_w_; }
    std::uint64_t cur_hash() const { return cur_h_; }
    int match_score() const { return match_score_; }
    int word_pos() const { return cur_n_ > 31 ? 31 : cur_n_; }
    int domain() const { return dom_; }

 private:
    struct Slot {
        std::uint64_t h = 0;
        std::uint64_t w[kWords] = {};
        int n = 0;
    };

    void rematch() {
        match_h_ = 0;
        match_score_ = 0;
        align_w_ = 0;
        if (cur_n_ < 2) return;
        const int ncur = cur_n_ > kWords ? kWords : cur_n_;
        int best = 0;
        std::uint64_t best_h = 0;
        std::uint64_t best_aw = 0;
        for (int i = 0; i < kRing; ++i) {
            const Slot& s = ring_[dom_][i];
            if (s.n < 2) continue;
            if (s.n > cur_n_ || s.n <= cur_n_ / 2) continue;
            int hit = 0;
            for (int k = 0; k < ncur; ++k) {
                for (int j = 0; j < s.n; ++j) {
                    if (cur_w_[k] == s.w[j]) {
                        ++hit;
                        break;
                    }
                }
            }
            if (hit > best) {
                best = hit;
                best_h = s.h;
                const int ai = (ncur - 1 < s.n) ? (ncur - 1) : (s.n - 1);
                best_aw = s.w[ai];
            }
        }
        if (best > 0 && best * 100 / cur_n_ >= 53) {
            match_h_ = best_h;
            match_score_ = best;
            align_w_ = best_aw;
        }
    }

    Slot ring_[kDom][kRing];
    int idx_[kDom] = {};
    int dom_ = 0;
    std::uint64_t cur_w_[kWords] = {};
    int cur_n_ = 0;
    std::uint64_t cur_h_ = 0;
    std::uint64_t match_h_ = 0;
    std::uint64_t align_w_ = 0;
    int match_score_ = 0;
};

}  // namespace hp
