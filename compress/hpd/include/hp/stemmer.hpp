#pragma once
//
// hp/stemmer.hpp — English Porter2 + POS-filtered stem streams (H2.1).
//
// Axis: morphology. The existing word_ / wstr_sp_ hash surface forms, so
// "compress" / "compressed" / "compressing" miss each other. Winners keep
// four stemmed rings filtered by POS (fx2 worcxt / worcxt1 / worcxt2).
//
// This is a compact integer port of the paq8pxd / fx2 EnglishStemmer:
// Porter2 regions + suffix steps, then closed-class POS lists. Not a copy
// of their 1b exception maze. Encoder and decoder run the same bytes.

#include <cstdint>
#include <cstring>

namespace hp {

enum StemPos : std::uint32_t {
    kPosVerb        = 1u << 0,
    kPosNoun        = 1u << 1,
    kPosAdj         = 1u << 2,
    kPosPlural      = 1u << 3,
    kPosPart        = 1u << 4,
    kPosPast        = 1u << 5,
    kPosAdvManner   = 1u << 6,
    kPosMale        = 1u << 7,
    kPosFemale      = 1u << 8,
    kPosArticle     = 1u << 9,
    kPosConj        = 1u << 10,
    kPosAdpos       = 1u << 11,
    kPosNumber      = 1u << 12,
    kPosConjAdv     = 1u << 13
};

static constexpr std::uint32_t kPosParaSkip =
    kPosConj | kPosArticle | kPosMale | kPosFemale | kPosNumber | kPosConjAdv;
static constexpr std::uint32_t kPosTypedSkip =
    kPosParaSkip | kPosAdpos | kPosAdvManner;

class StemWord {
 public:
    static constexpr int kMax = 62;

    void clear() {
        n_ = 0;
        type_ = 0;
        letters_[0] = 0;
    }
    void add(int c) {
        if (n_ >= kMax) return;
        if (c >= 'A' && c <= 'Z') c = c + 32;
        letters_[n_++] = static_cast<char>(c);
        letters_[n_] = 0;
    }
    int size() const { return n_; }
    char operator[](int i) const {
        return (i >= 0 && i < n_) ? letters_[i] : 0;
    }
    char back(int i = 0) const {
        return (n_ > i) ? letters_[n_ - 1 - i] : 0;
    }
    bool eq(const char* s) const { return std::strcmp(letters_, s) == 0; }
    bool ends(const char* s) const {
        const int m = static_cast<int>(std::strlen(s));
        return n_ >= m && std::memcmp(letters_ + n_ - m, s, static_cast<std::size_t>(m)) == 0;
    }
    bool starts(const char* s) const {
        const int m = static_cast<int>(std::strlen(s));
        return n_ >= m && std::memcmp(letters_, s, static_cast<std::size_t>(m)) == 0;
    }
    bool in(const char* const* a, int count) const {
        for (int i = 0; i < count; ++i)
            if (eq(a[i])) return true;
        return false;
    }
    void chop(int k) {
        if (k > n_) k = n_;
        n_ -= k;
        letters_[n_] = 0;
    }
    void replace_end(const char* old_s, const char* neu) {
        const int o = static_cast<int>(std::strlen(old_s));
        if (!ends(old_s)) return;
        chop(o);
        for (int i = 0; neu[i]; ++i) add(neu[i]);
    }
    std::uint32_t type() const { return type_; }
    void add_type(std::uint32_t t) { type_ |= t; }
    std::uint64_t hash() const {
        std::uint64_t h = 0xc01dflu;
        for (int i = 0; i < n_; ++i)
            h = h * 263ull * 8ull + static_cast<std::uint8_t>(letters_[i]);
        return h;
    }
    const char* c_str() const { return letters_; }

 private:
    char letters_[kMax + 1] = {0};
    int n_ = 0;
    std::uint32_t type_ = 0;
};

inline bool stem_vowel(char c) {
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y';
}

inline bool stem_has_vowel(const StemWord& w) {
    for (int i = 0; i < w.size(); ++i)
        if (stem_vowel(w[i])) return true;
    return false;
}

inline int stem_region(const StemWord& w, int from) {
    bool saw = false;
    for (int i = from; i < w.size(); ++i) {
        if (stem_vowel(w[i])) {
            saw = true;
            continue;
        }
        if (saw) return i + 1;
    }
    return w.size();
}

inline bool stem_in_r(const StemWord& w, int r, const char* suf) {
    const int m = static_cast<int>(std::strlen(suf));
    return w.size() >= m && r <= w.size() - m;
}

inline bool stem_short_syllable(const StemWord& w) {
    if (w.size() < 2) return false;
    if (w.size() == 2)
        return stem_vowel(w[0]) && !stem_vowel(w[1]);
    const char a = w.back(2), b = w.back(1), c = w.back(0);
    return !stem_vowel(a) && stem_vowel(b) && !stem_vowel(c) &&
           c != 'w' && c != 'x' && c != 'y';
}

inline bool stem_double(char c) {
    return c == 'b' || c == 'd' || c == 'f' || c == 'g' || c == 'm' ||
           c == 'n' || c == 'p' || c == 'r' || c == 't';
}

inline void stem_english(StemWord& w) {
    if (w.size() < 2) return;

    static const char* kMale[] = {
        "he", "him", "his", "himself", "man", "men", "boy", "husband", "actor"};
    static const char* kFemale[] = {
        "she", "her", "herself", "woman", "women", "girl", "wife", "actress"};
    static const char* kArt[] = {"a", "an", "the"};
    static const char* kConj[] = {
        "for", "and", "nor", "but", "or", "yet", "so", "than", "as", "that",
        "if", "when", "because", "while", "where", "after", "though",
        "whether", "before", "although", "like", "once", "unless", "now",
        "except"};
    static const char* kAdpos[] = {
        "in", "during", "at", "on", "since", "until", "above", "across",
        "against", "along", "among", "around", "behind", "below", "beneath",
        "beside", "between", "by", "down", "from", "into", "near", "of",
        "off", "to", "toward", "under", "upon", "with", "within"};
    static const char* kConjAdv[] = {"also", "thus"};
    static const char* kVerb[] = {
        "has", "had", "have", "was", "were", "may", "might", "must", "shall",
        "should", "can", "could", "will", "would", "is", "am", "are", "be",
        "being", "been", "do", "does", "did"};
    static const char* kNum[] = {
        "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
        "ten", "twenty", "thirty", "forty", "fifty", "sixty", "seventy",
        "eighty", "ninety", "hundred", "thousand", "million"};

    struct Pair { const char* a; const char* b; };
    static const Pair kEx1[] = {
        {"skis", "ski"}, {"skies", "sky"}, {"dying", "die"}, {"lying", "lie"},
        {"tying", "tie"}, {"idly", "idl"}, {"gently", "gentl"},
        {"ugly", "ugli"}, {"early", "earli"}, {"only", "onli"},
        {"singly", "singl"}};
    static const std::uint32_t kEx1T[] = {
        kPosNoun | kPosPlural, kPosNoun | kPosPlural, kPosPart, kPosPart,
        kPosPart, kPosAdvManner, kPosAdvManner, kPosAdj, kPosAdj | kPosAdvManner,
        0, kPosAdvManner};
    for (int i = 0; i < 11; ++i) {
        if (w.eq(kEx1[i].a)) {
            w.clear();
            for (int k = 0; kEx1[i].b[k]; ++k) w.add(kEx1[i].b[k]);
            w.add_type(kEx1T[i]);
            return;
        }
    }
    static const char* kEx2[] = {
        "inning", "outing", "canning", "herring", "earring",
        "proceed", "exceed", "succeed"};
    static const std::uint32_t kEx2T[] = {
        kPosNoun, kPosNoun, kPosNoun, kPosNoun, kPosNoun,
        kPosVerb, kPosVerb, kPosVerb};
    for (int i = 0; i < 8; ++i) {
        if (w.eq(kEx2[i])) {
            w.add_type(kEx2T[i]);
            return;
        }
    }

    if (w.ends("'s'") || w.ends("'s") || w.ends("'")) {
        if (w.ends("'s'")) w.chop(3);
        else if (w.ends("'s")) w.chop(2);
        else w.chop(1);
        w.add_type(kPosPlural);
    }

    int r1 = stem_region(w, 0);
    if (w.starts("gener") || w.starts("arsen") || w.starts("commun"))
        r1 = 5;
    const int r2 = stem_region(w, r1);

    if (w.ends("sses")) {
        w.chop(2);
        w.add_type(kPosPlural);
    } else if (w.ends("ied") || w.ends("ies")) {
        w.add_type(w.back(0) == 'd' ? kPosPast : kPosPlural);
        w.chop(w.size() > 4 ? 2 : 1);
    } else if (!w.ends("us") && !w.ends("ss") && w.back(0) == 's' && w.size() > 2) {
        for (int i = 0; i < w.size() - 2; ++i) {
            if (stem_vowel(w[i])) {
                w.chop(1);
                w.add_type(kPosPlural);
                break;
            }
        }
    }

    if (w.ends("eedly") && stem_in_r(w, r1, "eedly")) {
        w.chop(3);
    } else if (w.ends("eed") && stem_in_r(w, r1, "eed")) {
        w.chop(1);
    } else {
        const char* sufs[] = {"edly", "ed", "ingly", "ing"};
        const std::uint32_t ty[] = {
            kPosAdvManner | kPosPast, kPosPast,
            kPosAdvManner | kPosPart, kPosPart};
        for (int i = 0; i < 4; ++i) {
            if (!w.ends(sufs[i])) continue;
            const int keep = w.size() - static_cast<int>(std::strlen(sufs[i]));
            bool vow = false;
            for (int j = 0; j < keep; ++j)
                if (stem_vowel(w[j])) vow = true;
            if (!vow) break;
            w.chop(static_cast<int>(std::strlen(sufs[i])));
            w.add_type(ty[i] | kPosVerb);
            if (w.ends("at") || w.ends("bl") || w.ends("iz") ||
                (stem_short_syllable(w) && keep == w.size())) {
                w.add('e');
            } else if (w.size() >= 2 && w.back(0) == w.back(1) &&
                       stem_double(w.back(0))) {
                w.chop(1);
            }
            break;
        }
    }

    if (w.size() > 2 && w.back(0) == 'y' && !stem_vowel(w.back(1))) {
        w.chop(1);
        w.add('i');
    }

    struct Step { const char* a; const char* b; std::uint32_t t; };
    static const Step kS2[] = {
        {"ization", "ize", kPosNoun}, {"ational", "ate", kPosAdj},
        {"ousness", "ous", kPosNoun}, {"iveness", "ive", kPosNoun},
        {"fulness", "ful", kPosNoun}, {"tional", "tion", kPosAdj},
        {"lessli", "less", kPosAdvManner}, {"biliti", "ble", kPosNoun},
        {"entli", "ent", kPosAdvManner}, {"ation", "ate", kPosNoun},
        {"alism", "al", 0}, {"aliti", "al", kPosNoun},
        {"fulli", "ful", kPosAdvManner}, {"ousli", "ous", kPosAdvManner},
        {"iviti", "ive", kPosNoun}, {"enci", "ence", 0},
        {"anci", "ance", 0}, {"abli", "able", kPosAdvManner},
        {"izer", "ize", 0}, {"ator", "ate", 0},
        {"alli", "al", kPosAdvManner}, {"bli", "ble", kPosAdvManner}};
    for (const auto& s : kS2) {
        if (w.ends(s.a) && stem_in_r(w, r1, s.a)) {
            w.replace_end(s.a, s.b);
            w.add_type(s.t);
            break;
        }
    }
    static const Step kS3[] = {
        {"ational", "ate", kPosAdj}, {"tional", "tion", kPosAdj},
        {"alize", "al", 0}, {"icate", "ic", 0}, {"iciti", "ic", kPosNoun},
        {"ical", "ic", kPosAdj}, {"ful", "", kPosAdj}, {"ness", "", kPosNoun}};
    for (const auto& s : kS3) {
        if (w.ends(s.a) && stem_in_r(w, r1, s.a)) {
            w.replace_end(s.a, s.b);
            w.add_type(s.t);
            break;
        }
    }
    if (w.ends("ative") && stem_in_r(w, r2, "ative")) w.chop(5);
    if (w.size() > 5 && w.ends("less")) {
        w.chop(4);
        w.add_type(kPosAdj);
    }

    static const char* kS4[] = {
        "al", "ance", "ence", "er", "ic", "able", "ible", "ant", "ement",
        "ment", "ent", "ou", "ism", "ate", "iti", "ous", "ive", "ize",
        "sion", "tion"};
    static const std::uint32_t kS4T[] = {
        kPosAdj, 0, 0, 0, kPosAdj, 0, 0, 0, 0, 0, 0, 0, 0, 0, kPosNoun,
        kPosAdj, 0, 0, kPosNoun, kPosNoun};
    for (int i = 0; i < 20; ++i) {
        if (w.ends(kS4[i]) && stem_in_r(w, r2, kS4[i])) {
            w.chop(static_cast<int>(std::strlen(kS4[i])));
            w.add_type(kS4T[i]);
            break;
        }
    }

    if (w.back(0) == 'e' && !w.eq("here")) {
        if (stem_in_r(w, r2, "e"))
            w.chop(1);
        else if (stem_in_r(w, r1, "e")) {
            w.chop(1);
            if (stem_short_syllable(w)) w.add('e');
        }
    } else if (w.size() > 1 && w.back(0) == 'l' && w.back(1) == 'l' &&
               stem_in_r(w, r2, "l")) {
        w.chop(1);
    }

    if (!w.type() || w.type() == kPosPlural) {
        if (w.in(kMale, 9)) w.add_type(kPosMale);
        else if (w.in(kFemale, 8)) w.add_type(kPosFemale);
        else if (w.in(kArt, 3)) w.add_type(kPosArticle);
        else if (w.in(kConj, 25)) w.add_type(kPosConj);
        else if (w.in(kAdpos, 30)) w.add_type(kPosAdpos);
        else if (w.in(kConjAdv, 2)) w.add_type(kPosConjAdv);
        else if (w.in(kVerb, 23)) w.add_type(kPosVerb);
        else if (w.in(kNum, 21)) w.add_type(kPosNumber);
    }
}

// Three POS-filtered stem rings, as in fx2 worcxt / worcxt1 / worcxt2.
class StemStreams {
 public:
    void push(int byte, int nest) {
        const int c = byte;
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool apost = (c == '\'' && prev_ != '\'');
        const bool hyphen = (c == '-' && cur_.size() > 0);
        if (letter || apost || hyphen) {
            cur_.add(c);
        } else if (cur_.size() > 0) {
            stem_english(cur_);
            const std::uint64_t h = cur_.hash();
            const std::uint32_t t = cur_.type();
            prev_sen_ = sen_;
            sen_ = h;
            type_ = t;
            if ((t & kPosParaSkip) == 0) para_ = h;
            if (t && (t & kPosTypedSkip) == 0) typed_ = h;
            wt3_ = wt3_ * 0x100000001B3ull +
                   static_cast<std::uint64_t>(wt3_bucket(t)) +
                   static_cast<std::uint64_t>(wc_);
            if (wc_ < 63) ++wc_;
            cur_.clear();
        }
        const bool punct = (c == '.' || c == '!' || c == '?' || c == ';' ||
                            c == '\n');
        if (punct && !nest) {
            sen_ = 0;
            para_ = 0;
            wt3_ = 0;
            wc_ = 0;
        }
        prev_ = c;
    }

    std::uint64_t sentence() const { return sen_ ? sen_ : prev_sen_; }
    std::uint64_t paragraph() const { return para_; }
    std::uint64_t typed() const { return typed_; }
    std::uint32_t type() const { return type_; }
    std::uint64_t prev_sentence() const { return prev_sen_; }

    std::uint64_t ctx0() const {
        return sentence() * 3301ull + prev_sentence();
    }
    std::uint64_t ctx1() const {
        return typed() * 1471ull + static_cast<std::uint64_t>(type_ & 0xffu) +
               paragraph() * 17ull;
    }
    std::uint64_t wt3() const { return wt3_; }

 private:
    static int wt3_bucket(std::uint32_t t) {
        if (t & kPosVerb) return 1;
        if (t & kPosNoun) return 2;
        if (t & kPosAdj) return 3;
        if (t & kPosPlural) return 4;
        if (t & kPosPast) return 5;
        if (t & kPosPart) return 6;
        if (t & kPosAdvManner) return 10;
        if (t & kPosMale) return 13;
        if (t & kPosFemale) return 14;
        if (t & kPosArticle) return 15;
        if (t & kPosConj) return 16;
        if (t & kPosAdpos) return 17;
        if (t & kPosNumber) return 18;
        if (t & kPosConjAdv) return 20;
        if (t) return 14;
        return 15;
    }
    StemWord cur_;
    std::uint64_t sen_ = 0, prev_sen_ = 0, para_ = 0, typed_ = 0;
    std::uint32_t type_ = 0;
    int prev_ = 0;
    std::uint64_t wt3_ = 0;
    int wc_ = 0;
};

}  // namespace hp
