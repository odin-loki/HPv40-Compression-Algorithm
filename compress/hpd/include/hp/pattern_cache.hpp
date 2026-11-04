#pragma once
//
// hp/pattern_cache.hpp — B.1 derived-state cache.
//
// WHAT IT MAY DO
// --------------
// Memoize work that is a pure function of already-coded bytes: hash2() of a
// (salt, key) pair, and a 64-bit signature of the current pattern class.
// Encoder and decoder build the cache identically, so nothing is transmitted.
//
// WHAT IT MUST NOT DO
// -------------------
// Cache a *probability*. After every bit the tables move, so a stored p is
// stale on the next hit. Reusing it would change the archive. The acceptance
// test is bit-identity with the cache compiled out (HP_PATTERN_CACHE=0).
//
// Payoff is throughput, not ratio. Hits are counted so we can measure whether
// the lookup costs more than it saves (PLAN B.1 risk).

#include <cstdint>

#include "hp/models.hpp"

namespace hp {

class PatternCache {
 public:
    static constexpr int kLines = 32;

    std::uint32_t hash_memo(std::uint64_t salt, std::uint64_t key) {
        ++lookups_;
        const std::uint64_t sig = mix64(salt ^ (key * 0x9E3779B97F4A7C15ull));
        const int slot = static_cast<int>(sig) & (kLines - 1);
        if (line_[slot].valid && line_[slot].salt == salt &&
            line_[slot].key == key) {
            ++hits_;
            return line_[slot].out;
        }
        const std::uint32_t out = hash2(salt, key);
        line_[slot].salt = salt;
        line_[slot].key = key;
        line_[slot].out = out;
        line_[slot].valid = true;
        return out;
    }

    // Pattern-class taxonomy used as a mixer gate and as B.2 context.
    // Classes are detected from already-seen bytes only.
    enum Class : int {
        kPlain = 0,
        kTableRow,
        kCitation,
        kTimestamp,
        kInfobox,
        kLink,
        kMarkup,
        kNumeric,
        kNClass
    };

    void observe_byte(int byte, int wiki_state, int in_table, int first_class) {
        if (byte == '\n') {
            line_has_bar_ = 0;
            line_has_digit_ = 0;
            line_has_colon_ = 0;
            line_len_ = 0;
        } else {
            if (line_len_ < 4095) ++line_len_;
            if (byte == '|') line_has_bar_ = 1;
            if (byte >= '0' && byte <= '9') line_has_digit_ = 1;
            if (byte == ':') line_has_colon_ = 1;
        }
        // rolling 4-byte window for template / cite / http detection
        win_ = (win_ << 8) | static_cast<std::uint32_t>(byte & 0xff);

        cls_ = kPlain;
        if (in_table || line_has_bar_ || wiki_state == 7 /*kWkWikiTable*/ ||
            wiki_state == 8 /*kWkVerticalBar*/)
            cls_ = kTableRow;
        else if (wiki_state == 5 /*square*/ || wiki_state == 9 /*http*/)
            cls_ = kLink;
        else if (wiki_state == 6 /*curly*/)
            cls_ = kInfobox;
        else if (wiki_state == 1 || wiki_state == 2)
            cls_ = kMarkup;
        else if (line_has_digit_ && line_has_colon_ && line_len_ < 24)
            cls_ = kTimestamp;
        else if ((win_ & 0xffffff) == 0x726566 /* "ref" */)
            cls_ = kCitation;
        else if (first_class == 3)
            cls_ = kNumeric;

        ++class_count_[cls_];
        ++bytes_;
    }

    int cls() const { return cls_; }
    int lookups() const { return lookups_; }
    int hits() const { return hits_; }
    int class_count(int c) const { return class_count_[c]; }
    int bytes() const { return bytes_; }

    std::uint64_t signature(std::uint64_t hist, int c0, int mlen,
                            int wiki, int br) const {
        return mix64(hist ^ (static_cast<std::uint64_t>(c0) << 1) ^
                     (static_cast<std::uint64_t>(mlen) << 16) ^
                     (static_cast<std::uint64_t>(wiki) << 24) ^
                     (static_cast<std::uint64_t>(br) << 28) ^
                     (static_cast<std::uint64_t>(cls_) << 32));
    }

 private:
    struct Line {
        std::uint64_t salt = 0, key = 0;
        std::uint32_t out = 0;
        bool valid = false;
    };
    Line line_[kLines];
    int lookups_ = 0, hits_ = 0;
    int cls_ = 0;
    int line_has_bar_ = 0, line_has_digit_ = 0, line_has_colon_ = 0;
    int line_len_ = 0;
    int bytes_ = 0;
    int class_count_[kNClass] = {0};
    std::uint32_t win_ = 0;
};

}  // namespace hp
