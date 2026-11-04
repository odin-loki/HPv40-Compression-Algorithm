#pragma once
//
// hp/record.hpp — fixed-length record model (hpd).
//
// Tables, databases, spreadsheets, star catalogues and raw images repeat a
// record of r bytes; the byte one record back (the "cell above") is then the
// best context there is. HP40's column model only knows text lines. This model
// finds r online and offers three contexts on it.
//
// Detection (paq8-style): when a byte value recurs at the same distance d four
// times running, that is one vote for d. A period becomes active after enough
// votes for it, and is replaced only when another period out-votes it.
// Encoder and decoder see the same bytes, so they find the same r.

#include <cstdint>
#include <cstring>

#include "hp/models.hpp"
#include "hp/zero_array.hpp"

#ifndef HPD_RECORD
#define HPD_RECORD 1
#endif

namespace hp {

class RecordModel {
 public:
    static constexpr int kModels = 3;
    static constexpr int kOutputs = kModels * ContextModel::kOutputs;

    explicit RecordModel(int table_bits)
        : cm_{ContextModel(table_bits, 255), ContextModel(table_bits, 255), ContextModel(table_bits, 255)},
          ring_(kMask + 1) {
        std::memset(last_, 0, sizeof(last_));
        std::memset(d1_, 0, sizeof(d1_));
        std::memset(run_, 0, sizeof(run_));
        set_contexts();
    }

    int period() const { return r_; }
    // Position inside the current record, 0..r-1 (0 when no period is active).
    int column() const { return r_ ? static_cast<int>(pos_ % static_cast<std::uint64_t>(r_)) : 0; }

    void push(int c) {
        ring_[pos_ & kMask] = static_cast<std::uint8_t>(c);
        const std::uint64_t d = pos_ - last_[c];
        if (last_[c] && d > 1 && d <= kMaxPeriod) {
            if (d == d1_[c]) { if (run_[c] < 255) ++run_[c]; }
            else { d1_[c] = static_cast<std::uint32_t>(d); run_[c] = 0; }
            if (run_[c] >= 3) vote(static_cast<int>(d));
        }
        last_[c] = pos_;
        ++pos_;
        set_contexts();
    }

    void prefetch_nibble(int c0) { for (auto& m : cm_) m.prefetch_nibble(c0); }
    void predict(int c0, int backoff_p12, int* out) {
        for (int i = 0; i < kModels; ++i) cm_[i].predict(c0, backoff_p12, out + i * ContextModel::kOutputs);
    }
    void update(int y, int ens) { for (auto& m : cm_) m.update(y, ens); }

 private:
    static constexpr std::uint64_t kMask = (1u << 17) - 1;   // ring of 128 KiB
    static constexpr std::uint64_t kMaxPeriod = 1u << 15;

    int at(std::uint64_t back) const { return back <= pos_ ? ring_[(pos_ - back) & kMask] : 0; }

    void vote(int d) {
        if (d == cand_) { if (++cand_votes_ > 16 && d != r_) { r_ = d; } }
        else if (cand_votes_ > 0) --cand_votes_;
        else { cand_ = d; cand_votes_ = 1; }
    }

    static std::uint32_t h(std::uint64_t a, std::uint64_t b) {
        std::uint64_t x = a * 0x9E3779B97F4A7C15ull ^ (b + 0x632BE59BD9B4E019ull);
        x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 32;
        return static_cast<std::uint32_t>(x);
    }

    void set_contexts() {
        if (!r_) { for (auto& m : cm_) m.set_idle(); return; }
        const std::uint64_t r = static_cast<std::uint64_t>(r_);
        const int above = at(r), above2 = at(2 * r), left = at(1);
        const int col = column() > 4095 ? 4095 : column();
        cm_[0].set_context(h(0x5245431ull + r, static_cast<std::uint64_t>(above) << 8 | above2));
        cm_[1].set_context(h(0x5245432ull + r, static_cast<std::uint64_t>(above) << 8 | left));
        cm_[2].set_context(h(0x5245433ull + r, static_cast<std::uint64_t>(col) << 8 | above));
    }

    ContextModel cm_[kModels];
    ZeroArray<std::uint8_t> ring_;
    std::uint64_t pos_ = 0;
    std::uint64_t last_[256];
    std::uint32_t d1_[256];
    std::uint8_t run_[256];
    int cand_ = 0, cand_votes_ = 0, r_ = 0;
};

}  // namespace hp
