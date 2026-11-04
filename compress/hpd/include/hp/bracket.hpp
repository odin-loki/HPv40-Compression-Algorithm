#pragma once
//
// hp/bracket.hpp — nesting-depth model, 256 contexts, depth 15.
//
// Axis: bracket / nesting. Distinct from XML tag depth (wiki.hpp) because it
// tracks (), [], {}, <> independently and so fires inside math, wiki links,
// templates and C-like fragments that the tag machine never sees.

#include <cstdint>

namespace hp {

class BracketMachine {
 public:
    void push(int byte) {
        if (byte == '\'' && last_ == '\'') quote_ ^= 1;
        if (byte == '"' && last_ == '"') quote_ ^= 2;
        last_ = byte;
        switch (byte) {
            case '(': bump(0, +1); break;
            case ')': bump(0, -1); break;
            case '[': bump(1, +1); break;
            case ']': bump(1, -1); break;
            case '{': bump(2, +1); break;
            case '}': bump(2, -1); break;
            case '<': bump(3, +1); break;
            case '>': bump(3, -1); break;
            default: break;
        }
    }

    // 8-bit context: four 2-bit depths + last-bracket class in the high bits
    // of the hash key (the ContextModel hashes this further).
    std::uint64_t context_key() const {
        const int d0 = depth_[0] > 3 ? 3 : depth_[0];
        const int d1 = depth_[1] > 3 ? 3 : depth_[1];
        const int d2 = depth_[2] > 3 ? 3 : depth_[2];
        const int d3 = depth_[3] > 3 ? 3 : depth_[3];
        return static_cast<std::uint64_t>(
            d0 | (d1 << 2) | (d2 << 4) | (d3 << 6) | ((last_ & 0xff) << 8));
    }

    int total_depth() const {
        int t = depth_[0] + depth_[1] + depth_[2] + depth_[3];
        return t > 15 ? 15 : t;
    }
    int square_depth() const { return depth_[1]; }
    int curly_depth() const { return depth_[2]; }
    int quote() const { return quote_; }
    int closer() const {
        if (depth_[0]) return ')';
        if (depth_[1]) return ']';
        if (depth_[2]) return '}';
        if (depth_[3]) return '>';
        return 0;
    }

 private:
    void bump(int i, int dir) {
        depth_[i] += dir;
        if (depth_[i] < 0) depth_[i] = 0;
        if (depth_[i] > 15) depth_[i] = 15;
    }
    int depth_[4] = {0, 0, 0, 0};
    int last_ = 0;
    int quote_ = 0;
};

}  // namespace hp
