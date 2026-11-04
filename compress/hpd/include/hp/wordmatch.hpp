#pragma once
//
// hp/wordmatch.hpp — match models keyed on word context, not byte context.
//
// hp's byte-keyed match bank already occupies four of the six most
// decorrelated slots. A word-keyed variant reads a different axis: "the last
// time we were in this phrase, what byte came next", which fires on repeated
// *word* sequences whose byte suffixes have already diverged (inflected
// endings, different markup around the same tokens).
//
// order = how many completed words are hashed. Smaller tables than the byte
// match bank — this is a specialist.

#include <cstdint>
#include <vector>

#include "hp/models.hpp"
#include "hp/zero_array.hpp"

namespace hp {

class WordMatchModel {
 public:
    WordMatchModel(int buf_bits, int table_bits, int word_order,
                   std::shared_ptr<HistoryBuf> hist = nullptr)
        : order_(word_order < 1 ? 1 : word_order),
          buf_bits_(buf_bits),
          hist_(hist ? std::move(hist) : std::make_shared<HistoryBuf>(buf_bits)),
          buf_mask_(hist_->mask),
          tab_mask_((1u << table_bits) - 1),
          buf_(hist_->a.data()),
          tab_(static_cast<std::size_t>(1) << table_bits),
          st_(64) {
        counter_init(st_.data(), st_.size());
    }

    // byte is the byte just completed. whist is the rolling hash of the last
    // `order_` completed words (0 if we have not yet seen that many).
    void push_byte(int byte, std::uint64_t whist, int at_word_boundary) {
        if (len_ > 0) {
            if (ptr_ < pos_ && buf_[ptr_ & buf_mask_] == static_cast<std::uint8_t>(byte)) {
                if (len_ < 65535) ++len_;
                ++ptr_;
            } else {
                len_ = 0;
            }
        }
        buf_[pos_ & buf_mask_] = static_cast<std::uint8_t>(byte);
        ++pos_;

        if (at_word_boundary && whist != 0) {
            const std::uint32_t h =
                hash2(0x574D0000ull + static_cast<std::uint64_t>(order_), whist) &
                tab_mask_;
            if (len_ == 0) {
                const std::uint32_t cand = tab_[h];
                if (cand > 0 && cand < pos_) {
                    ptr_ = cand;
                    len_ = 1;
                }
            }
            tab_[h] = pos_;
        }
        if (len_ > 0 && (pos_ - ptr_) > buf_mask_) len_ = 0;
    }

    int predict(int c0, int bitpos) {
        valid_ = false;
        if (len_ == 0 || ptr_ >= pos_) return 0;
        const int pred_byte = buf_[ptr_ & buf_mask_];
        if (bitpos > 0) {
            if (((pred_byte | 0x100) >> (8 - bitpos)) != c0) {
                len_ = 0;
                return 0;
            }
        }
        expected_ = (pred_byte >> (7 - bitpos)) & 1;
        const int lq = len_ > 31 ? 31 : len_;
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
    std::shared_ptr<HistoryBuf> hist_;   // must precede buf_mask_ / buf_
    std::uint32_t buf_mask_;
    std::uint32_t tab_mask_;
    std::uint8_t* buf_;
    ZeroArray<std::uint32_t> tab_;
    std::vector<Counter> st_;
    std::uint32_t pos_ = 0, ptr_ = 0;
    int len_ = 0, expected_ = 0, sidx_ = 0;
    bool valid_ = false;
};

}  // namespace hp
