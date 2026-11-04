#pragma once
//
// hp/wordstream.hpp — four word streams + first-char / capitalisation axes.
//
// fx2-cmix keeps four independent word streams and ten sparse configurations
// over them. hp had one folded word hash and one word-bigram. This file adds
// the missing streams without copying their table sizes:
//
//   0  folded alnum          (same axis as the existing word_ model)
//   1  case-preserving       capitalisation state
//   2  first-character class lower / upper / digit / other
//   3  sentence stream       words after . ! ? or newline
//
// Sparse configs actually wired (A.2.c, trimmed to what the mixer can digest):
//   stream1 unigram,  stream3 unigram,  stream0[-1] + stream2 (skip)

#include <cstdint>

#include "hp/models.hpp"

namespace hp {

inline int word_first_class(int byte) {
    if (byte >= 'a' && byte <= 'z') return 1;
    if (byte >= 'A' && byte <= 'Z') return 2;
    if (byte >= '0' && byte <= '9') return 3;
    return 0;
}

class WordStreams {
 public:
    void push(int byte, bool alnum) {
        if (alnum) {
            if (cur_len_ == 0) {
                first_class_ = word_first_class(byte);
                capital_ = (byte >= 'A' && byte <= 'Z') ? 1 : 0;
            }
            s0_ = mix64(s0_ * 0x100000001B3ull +
                        static_cast<std::uint64_t>(byte | 0x20));
            s1_ = mix64(s1_ * 0x100000001B3ull +
                        static_cast<std::uint64_t>(byte));
            if (cur_len_ < 63) ++cur_len_;
        } else {
            if (cur_len_ > 0) {
                prev0_ = s0_;
                prev1_ = s1_;
                if (sentence_) {
                    s3_ = s0_;
                    first_word_ = s0_;
                    sentence_ = 0;
                }
                recency_push(s0_);
                s2_ = static_cast<std::uint64_t>(first_class_) +
                      (static_cast<std::uint64_t>(capital_) << 3) +
                      (static_cast<std::uint64_t>(cur_len_ > 8 ? 8 : cur_len_) << 4);
            }
            s0_ = 0;
            s1_ = 0;
            cur_len_ = 0;
            if (byte == '.' || byte == '!' || byte == '?' || byte == '\n') {
                sentence_ = 1;
                recency_end();
            }
        }
    }

    std::uint64_t stream(int i) const {
        switch (i) {
            case 1: return s1_ ? s1_ : prev1_;
            case 2: return s2_;
            case 3: return s3_;
            default: return s0_ ? s0_ : prev0_;
        }
    }
    std::uint64_t prev0() const { return prev0_; }
    int first_class() const { return first_class_; }
    int capital() const { return capital_; }
    int word_len() const { return cur_len_; }
    std::uint64_t first_word() const { return first_word_; }
    std::uint64_t recency_at() const {
        const int i = rec_pos_;
        if (i >= 0 && i < rec_prev_n_ && i < 32) return rec_prev_[i];
        return 0;
    }
    int first_word_bin() const {
        return static_cast<int>(first_word_ & 15ull);
    }
    int sent_pos() const { return rec_pos_ > 31 ? 31 : rec_pos_; }

 private:
    void recency_push(std::uint64_t h) {
        if (rec_cur_n_ < 32) rec_cur_[rec_cur_n_++] = h;
        if (rec_pos_ < 255) ++rec_pos_;
    }
    void recency_end() {
        for (int i = 0; i < 32; ++i)
            rec_prev_[i] = (i < rec_cur_n_) ? rec_cur_[i] : 0;
        rec_prev_n_ = rec_cur_n_;
        rec_cur_n_ = 0;
        rec_pos_ = 0;
    }
    std::uint64_t s0_ = 0, s1_ = 0, s2_ = 0, s3_ = 0;
    std::uint64_t prev0_ = 0, prev1_ = 0;
    int cur_len_ = 0;
    int first_class_ = 0;
    int capital_ = 0;
    int sentence_ = 1;
    std::uint64_t first_word_ = 0;
    std::uint64_t rec_cur_[32] = {};
    std::uint64_t rec_prev_[32] = {};
    int rec_cur_n_ = 0;
    int rec_prev_n_ = 0;
    int rec_pos_ = 0;
};

}  // namespace hp
