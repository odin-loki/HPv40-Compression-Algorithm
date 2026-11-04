#pragma once
//
// hp/numeric.hpp — digit-run / decimal-field axis (fx2 number0/numlen).
//
// Distinct from the word model: years, table cells, versions, and
// timestamps share a shape that byte-suffix models fragment.

#include <cstdint>

namespace hp {

class NumericField {
 public:
    void push(int byte) {
        const int d = (byte >= '0' && byte <= '9') ? (byte - '0') : -1;
        if (d >= 0) {
            if (len0_ < 19) {
                n0_ = n0_ * 10ull + static_cast<std::uint64_t>(d);
                ++len0_;
            }
            if (prev_ == '.' ) dec_ = 1;
            else if (prev_ == ',') dec_ = 2;
        } else {
            if (len0_ > 0) {
                n1_ = n0_;
                len1_ = len0_;
                n0_ = 0;
                len0_ = 0;
                dec_ = 0;
            }
        }
        prev_ = byte;
    }

    std::uint64_t context_key() const {
        const int l0 = len0_ > 15 ? 15 : len0_;
        const int l1 = len1_ > 15 ? 15 : len1_;
        // Low bits of the current and previous numbers + lengths + decimal class.
        return (n0_ & 0xffffull) |
               ((n1_ & 0xffffull) << 16) |
               (static_cast<std::uint64_t>(l0) << 32) |
               (static_cast<std::uint64_t>(l1) << 36) |
               (static_cast<std::uint64_t>(dec_ & 3) << 40);
    }

    int length() const { return len0_; }
    std::uint64_t current() const { return n0_; }
    std::uint64_t previous() const { return n1_; }

 private:
    std::uint64_t n0_ = 0, n1_ = 0;
    int len0_ = 0, len1_ = 0, dec_ = 0, prev_ = 0;
};

}  // namespace hp
