#pragma once
//
// hp/dict.hpp — dictionary preprocessing.
//
// THE MULTIPLICATIVE IDEA
// -----------------------
// Replace frequent words with short tokens BEFORE modelling. This is exactly
// the "swap table" instinct, correctly accounted: the transform is bijective
// and therefore removes no entropy at all. The gain is entirely that it
// extends every model's context reach at once.
//
// An order-6 model over raw text spans "encycl". Over tokenised text the same
// six bytes can span three whole words. Every context model, the match model,
// and the word model all get deeper simultaneously -- multiplicative, not
// additive. This is what took Rhatushnyak's first entry 6.8% over baseline.
//
// SIZE ACCOUNTING
// ---------------
// S = S1 + S2, so a shipped dictionary counts against the score. We therefore
// BUILD the dictionary from the input in a first pass and EMBED it in the
// archive. It costs what it costs, honestly, and the profiler reports whether
// it paid for itself. No outside information enters -- everything the
// decompressor needs is in the stream, which is what the rules require.
//
// TOKEN ENCODING
// --------------
// Tokens use bytes 0x01..0x05 as escape prefixes over a byte payload, giving
// 5 * 256 = 1280 slots. Byte 0x06 escapes a literal occurrence of any of
// 0x01..0x06 in the source. enwik is XML text so these control bytes are
// rare, making the escape cost negligible.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace hp {

constexpr int kDictEsc0 = 0x01;   // first escape byte
constexpr int kDictNEsc = 5;      // escapes 0x01..0x05
constexpr int kDictLiteral = 0x06;
constexpr int kDictMaxWords = kDictNEsc * 256;
constexpr int kDictMinLen = 4;    // shorter words cost more than they save
constexpr int kDictMinCount = 8;

inline bool dict_is_word_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Build a dictionary from the data: the highest-value words by
// (len - 2) * (count - 1), which is the actual byte saving of tokenising.
inline std::vector<std::string> dict_build(const std::vector<std::uint8_t>& in) {
    std::unordered_map<std::string, std::uint32_t> freq;
    freq.reserve(1 << 18);
    std::string cur;
    for (std::size_t i = 0; i <= in.size(); ++i) {
        const bool wb = i < in.size() && dict_is_word_byte(in[i]);
        if (wb) {
            if (cur.size() < 40) cur.push_back(static_cast<char>(in[i]));
        } else {
            if (cur.size() >= static_cast<std::size_t>(kDictMinLen)) ++freq[cur];
            cur.clear();
        }
    }
    struct Cand { std::string w; std::int64_t gain; };
    std::vector<Cand> cands;
    cands.reserve(freq.size());
    for (auto& kv : freq) {
        if (kv.second < static_cast<std::uint32_t>(kDictMinCount)) continue;
        const std::int64_t gain =
            static_cast<std::int64_t>(kv.first.size() - 2) * (kv.second - 1);
        if (gain > 0) cands.push_back({kv.first, gain});
    }
    // Deterministic ordering: gain desc, then lexicographic. The tie-break
    // matters -- unordered_map iteration order is not portable, and the
    // dictionary must be identical on every machine.
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.gain != b.gain) return a.gain > b.gain;
        return a.w < b.w;
    });
    if (cands.size() > static_cast<std::size_t>(kDictMaxWords))
        cands.resize(kDictMaxWords);
    std::vector<std::string> out;
    out.reserve(cands.size());
    for (auto& c : cands) out.push_back(c.w);
    return out;
}

inline void dict_encode(const std::vector<std::uint8_t>& in,
                        const std::vector<std::string>& dict,
                        std::vector<std::uint8_t>& out) {
    std::unordered_map<std::string, int> id;
    id.reserve(dict.size() * 2);
    for (std::size_t i = 0; i < dict.size(); ++i) id[dict[i]] = static_cast<int>(i);

    out.clear();
    out.reserve(in.size());
    std::size_t i = 0;
    std::string cur;
    while (i < in.size()) {
        if (dict_is_word_byte(in[i])) {
            std::size_t j = i;
            cur.clear();
            while (j < in.size() && dict_is_word_byte(in[j]) && cur.size() < 40) {
                cur.push_back(static_cast<char>(in[j]));
                ++j;
            }
            auto it = id.find(cur);
            if (it != id.end()) {
                const int t = it->second;
                out.push_back(static_cast<std::uint8_t>(kDictEsc0 + (t >> 8)));
                out.push_back(static_cast<std::uint8_t>(t & 0xff));
            } else {
                out.insert(out.end(), cur.begin(), cur.end());
            }
            i = j;
        } else {
            const std::uint8_t c = in[i++];
            if (c >= kDictEsc0 && c <= kDictLiteral) out.push_back(kDictLiteral);
            out.push_back(c);
        }
    }
}

inline void dict_decode(const std::vector<std::uint8_t>& in,
                        const std::vector<std::string>& dict,
                        std::vector<std::uint8_t>& out) {
    out.clear();
    out.reserve(in.size() * 2);
    std::size_t i = 0;
    while (i < in.size()) {
        const std::uint8_t c = in[i];
        if (c == kDictLiteral) {
            if (i + 1 < in.size()) out.push_back(in[i + 1]);
            i += 2;
        } else if (c >= kDictEsc0 && c < kDictEsc0 + kDictNEsc) {
            if (i + 1 >= in.size()) break;
            const int t = (c - kDictEsc0) * 256 + in[i + 1];
            if (t < static_cast<int>(dict.size())) {
                const std::string& w = dict[t];
                out.insert(out.end(), w.begin(), w.end());
            }
            i += 2;
        } else {
            out.push_back(c);
            ++i;
        }
    }
}

// Serialised dictionary: u16 count, then length-prefixed words.
inline void dict_serialise(const std::vector<std::string>& dict,
                           std::vector<std::uint8_t>& out) {
    out.clear();
    out.push_back(static_cast<std::uint8_t>(dict.size() >> 8));
    out.push_back(static_cast<std::uint8_t>(dict.size() & 0xff));
    for (const auto& w : dict) {
        out.push_back(static_cast<std::uint8_t>(w.size()));
        out.insert(out.end(), w.begin(), w.end());
    }
}

inline std::size_t dict_deserialise(const std::vector<std::uint8_t>& in,
                                    std::vector<std::string>& dict) {
    dict.clear();
    if (in.size() < 2) return 0;
    const std::size_t n = (static_cast<std::size_t>(in[0]) << 8) | in[1];
    std::size_t p = 2;
    for (std::size_t k = 0; k < n && p < in.size(); ++k) {
        const std::size_t len = in[p++];
        if (p + len > in.size()) break;
        dict.emplace_back(reinterpret_cast<const char*>(&in[p]), len);
        p += len;
    }
    return p;
}

}  // namespace hp
