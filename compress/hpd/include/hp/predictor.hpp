#pragma once
//
// hp/predictor.hpp — the model stack.
//
// Per bit:
//   1. every expert emits a stretched opinion
//   2. the mixer combines them, gated by (GRIA alpha bucket, partial byte, …)
//   3. two/three APM stages recalibrate
//   4. the coder codes the bit with the final probability
//   5. everything updates on the observed bit
//
// Steps 1-3 and 5 are IDENTICAL in the compressor and the decompressor, and
// depend only on data both sides already have.

#include "hp/hd.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>

#include "hp/bracket.hpp"
#include "hp/discover.hpp"
#include "hp/english.hpp"
#include "hp/features.hpp"
#include "hp/gria.hpp"
#include "hp/hedge.hpp"
#include "hp/int_math.hpp"
#include "hp/mixer.hpp"
#include "hp/models.hpp"
#include "hp/record.hpp"
#include "hp/numeric.hpp"
#include "hp/pattern_cache.hpp"
#include "hp/stat_gates.hpp"
#include "hp/statemap.hpp"
#include "hp/wiki.hpp"
#include "hp/stemmer.hpp"
#include "hp/wordmatch.hpp"
#include "hp/wordstream.hpp"
#include "hp/sentmem.hpp"


#define HP_TINY_BITS 16  // table bits for a model that is switched off

namespace hp {

#ifndef HPD_APM2
#define HPD_APM2 0
#endif
#ifndef HPD_WA2
#define HPD_WA2 1
#endif
#ifndef HPD_WB2
#define HPD_WB2 4
#endif
#ifndef HPD_WG2
#define HPD_WG2 1
#endif
#ifndef HPD_WO2
#define HPD_WO2 2
#endif

// hpd: per-model table cap, relative to table_bits. HP40 let the word tables
// grow 9 bits past the base (2^31 slots at --mem 22). Held by each Predictor
// (slot_cap_ = table_bits + slot_extra) and read only while its models are built.

struct Config {
    int table_bits = 22;
    int slot_extra = 32;    // max bits a model may grow above table_bits (32 = HP40 behaviour)
    int disc_slots = HP_DISC_SLOTS;   // discovery-pool slots (1..12)
    int buf_bits = 26;
    int match_bits = 22;
    int mixer_lr = 2;
    bool gria = true;

    // Encoder and decoder must agree. match/buf sizes are a function of
    // table_bits (the only size the archive header carries).
    // table_bits + 4 restores the historical default (22 -> 26).
    void normalize() {
        if (table_bits < 16) table_bits = 16;
        if (table_bits > 28) table_bits = 28;
        match_bits = table_bits;
        buf_bits = table_bits + 4;
        if (buf_bits > 28) buf_bits = 28;
    }
};

struct WarpOps;

class Predictor {
    friend struct WarpOps;   // hpd-gpu: warp-parallel predict / update
 public:
    static constexpr int kExtraCtx =
        (HP_WORD_STREAMS ? 1 : 0) +
        (HP_BRACKET ? 1 : 0) +
        (HP_LINKWORD ? 1 : 0) +
        (HP_NUMERIC ? 1 : 0) +
        (HP_PAT_MODEL ? 1 : 0) +
        (HP_PPMD ? 1 : 0) +
        (HP_STEMMER ? HP_STEMMER_N : 0) +
        (HP_SENWORD ? 1 : 0) +
        (HP_SENT_STREAM ? 1 : 0) +
        (HP_SENT_MEM ? 1 : 0);
    static constexpr int kCtxModels = 6 + (HP_CM_SP ? 2 : 0) + (HP_CM_COL ? 1 : 0)
        + (HP_CM_TAG ? 1 : 0) + (HP_CM_WBI ? 1 : 0) + kExtraCtx;
    static constexpr int kMatchModels = 5 + (HP_MATCH_18 ? 1 : 0)
        + (HP_MATCH_13 ? 1 : 0) + (HP_MATCH_01 ? 1 : 0)
        + (HP_MATCH_02 ? 1 : 0) + (HP_MATCH_05 ? 1 : 0)
        + (HP_MATCH_07 ? 1 : 0) + (HP_MATCH_09 ? 1 : 0)
        + (HP_MATCH_12 ? 1 : 0) + (HP_MATCH_20 ? 1 : 0);
    static constexpr int kWordMatch = HP_WORD_MATCH
        ? (HP_WORD_MATCH_N + (HP_WMATCH_4 ? 1 : 0) + (HP_WMATCH_5 ? 1 : 0)) : 0;
    static constexpr int kCtw = HP_CTW ? 1 : 0;
    static constexpr int kDiscovered =
        DiscoveryPool::kSlots * ContextModel::kOutputs;
    static constexpr int kBaseExperts =
        kCtxModels * ContextModel::kOutputs + 1 /*bias*/ + kMatchModels
        + kWordMatch + (HP_SPARSE_UTF8 ? 1 : 0)
        + (HP_HEBB_EXPERT ? 1 : 0) /*hebb*/ + kCtw + kDiscovered
        + (HPD_RECORD ? RecordModel::kOutputs : 0);
    static constexpr int kHedgeInputs = 0;
    static constexpr int kNumExperts = kBaseExperts + kHedgeInputs;

    enum Gate {
        kGateC0 = 0, kGateAlpha, kGatePrev, kGateMatch, kGateEntropy,
        kGateHebb,
        kGateWiki, kGatePattern,
        kGateArgmax,
        kGateWordPos,
#if HPD_REC_GATE
        kGateRecord,
#endif
        kNumGates
    };

    int slot_bits(int base, int delta) const {
        if (delta < 0) delta = 0;
        int b = base + delta;
        if (delta >= 2) b += 1;
        if (delta == 1) b += 1;
        if (delta == 1) b += 1;
        if (delta == 1) b += 1;
        if (delta == 1) b += 1;
        if (delta == 1) b += 1;
        if (delta == 1) b += 1;
        if (delta == 1) b += 1;
        if (delta == 1) b += 1;
        if (b < 16) b = 16;
        if (b > HP_SLOT_MAX) b = HP_SLOT_MAX;
        if (b > slot_cap_) b = slot_cap_;
        return b;
    }

    static int match_bits(int b) {
        b += (HP_MATCH_GROW ? 1 : 0) + (HP_MATCH_GROW2 ? 1 : 0);
        if (b < 16) b = 16;
        if (b > 28) b = 28;
        return b;
    }

    int add_bits(int b, int extra) const {
        b += extra;
        if (b < 16) b = 16;
        if (b > HP_SLOT_MAX) b = HP_SLOT_MAX;
        if (b > slot_cap_) b = slot_cap_;
        return b;
    }

    explicit Predictor(const Config& cfg)
        : cfg_(cfg),
          slot_cap_(cfg.table_bits + cfg.slot_extra),
          histbuf_(make_shared_hd<HistoryBuf>(cfg.buf_bits)),
#if HPD_RECORD
          rec_(slot_bits(cfg.table_bits, 0)),
#endif
          o1_(add_bits(slot_bits(cfg.table_bits, -2), HP_SLOT_O12 ? 1 : 0), 1023),
          o2_(add_bits(slot_bits(cfg.table_bits, -2), HP_SLOT_O12 ? 1 : 0), 1023),
          o3_(add_bits(slot_bits(cfg.table_bits, 0),
                       (HP_SLOT_O34 ? 1 : 0) + (HP_SLOT_O34B ? 1 : 0)
                       + (HP_SLOT_O34C ? 1 : 0) + (HP_SLOT_O34D ? 1 : 0)
                       + (HP_SLOT_O34E ? 1 : 0) + (HP_SLOT_O34F ? 1 : 0)
                       + (HP_SLOT_O34G ? 1 : 0)), 511),
          o4_(add_bits(slot_bits(cfg.table_bits, 0),
                       (HP_SLOT_O34 ? 1 : 0) + (HP_SLOT_O34B ? 1 : 0)
                       + (HP_SLOT_O34C ? 1 : 0) + (HP_SLOT_O34D ? 1 : 0)
                       + (HP_SLOT_O34E ? 1 : 0) + (HP_SLOT_O34F ? 1 : 0)
                       + (HP_SLOT_O34G ? 1 : 0)), 255),
          o6_(add_bits(slot_bits(cfg.table_bits, 0),
                       (HP_SLOT_O6 ? 1 : 0) + (HP_SLOT_O6B ? 1 : 0)
                       + (HP_SLOT_O6C ? 1 : 0) + (HP_SLOT_O6D ? 1 : 0)
                       + (HP_SLOT_O6E ? 1 : 0) + (HP_SLOT_O6F ? 1 : 0)
                       + (HP_SLOT_O6G ? 1 : 0) + (HP_SLOT_O6H ? 1 : 0)
                       + (HP_SLOT_O6I ? 1 : 0) + (HP_SLOT_O6J ? 1 : 0)), 127),
          word_(slot_bits(cfg.table_bits, 1), 255),
          col_(HP_CM_COL ? slot_bits(cfg.table_bits, HP_SLOT_COL2 ? 2 : 0) : HP_TINY_BITS, 255),
          tag_(HP_CM_TAG ? slot_bits(cfg.table_bits, 2) : HP_TINY_BITS, 255),
          wbi_(HP_CM_WBI ? slot_bits(cfg.table_bits, 1) : HP_TINY_BITS, 255),
          sp13_(HP_CM_SP ? add_bits(slot_bits(cfg.table_bits, 0), HP_SLOT_SP ? 1 : 0) : HP_TINY_BITS, 255),
          sp24_(HP_CM_SP ? add_bits(slot_bits(cfg.table_bits, 0), HP_SLOT_SP ? 1 : 0) : HP_TINY_BITS, 255),
          brk_(slot_bits(cfg.table_bits, HP_SLOT_BRK2 ? 2 : 0), 255),
          sentst_(slot_bits(cfg.table_bits, HP_SLOT_S3 ? (HP_SLOT_S4 ? 3 : 2) : 0), 255),
          match_{ {cfg.buf_bits, match_bits(cfg.match_bits), 3, histbuf_},
                  {cfg.buf_bits, match_bits(cfg.match_bits), 4, histbuf_},
                  {cfg.buf_bits, match_bits(cfg.match_bits), 6, histbuf_},
                  {cfg.buf_bits, match_bits(cfg.match_bits), 10, histbuf_},
                  {cfg.buf_bits, match_bits(cfg.match_bits), 16, histbuf_}
                  , {cfg.buf_bits, match_bits(cfg.match_bits), 1, histbuf_}
                  , {cfg.buf_bits, match_bits(cfg.match_bits), 2, histbuf_}
          },
          smatch_(cfg.buf_bits, match_bits(cfg.match_bits), 4, histbuf_),
          wmatch_{
              WordMatchModel(cfg.buf_bits > 2 ? cfg.buf_bits - 2 : cfg.buf_bits,
                             cfg.match_bits > 2 ? cfg.match_bits - 2 : cfg.match_bits, 1, histbuf_),
              WordMatchModel(cfg.buf_bits > 2 ? cfg.buf_bits - 2 : cfg.buf_bits,
                             cfg.match_bits > 2 ? cfg.match_bits - 2 : cfg.match_bits, 2, histbuf_),
              WordMatchModel(cfg.buf_bits > 2 ? cfg.buf_bits - 2 : cfg.buf_bits,
                             cfg.match_bits > 2 ? cfg.match_bits - 2 : cfg.match_bits, 3, histbuf_)
          },
          hebb_(cfg.table_bits, 255),
          pool_(cfg.table_bits, 0xC0FFEEull, cfg.disc_slots),
          mixer_(kNumExperts, gate_sizes(), 256, cfg.mixer_lr, gate_rates(cfg.mixer_lr)),
          apm_c0_(256),
          apm_lex_(256 * 256),
          apm_gria_(GriaGate::kBuckets * 256),
#if HPD_APM2
          apm_o2_(1 << 16),
#endif
          hedge_(kNumGates),
          bias_(256) {
        counter_init(bias_.data(), bias_.size());
        for (int i = 0; i < 256; ++i) {
            bias_[static_cast<std::size_t>(i)].p =
                static_cast<std::uint16_t>(english_bit_prior16(i));
        }
        gria_.set_enabled(cfg.gria);
        set_byte_contexts();
    }

    int predict() {
        mixer_.reset_inputs();
        mixer_.add(counter_predict(bias_[c0_]));
        exp_p_[0] = counter_predict_p(bias_[c0_]);

        n_exp_ = 0;
        int out[ContextModel::kOutputs];
        int backoff = counter_predict_p(bias_[c0_]);
        ContextModel* chain[kCtxModels];
        int nchain = 0;
        chain[nchain++] = &o1_;
        chain[nchain++] = &o2_;
        chain[nchain++] = &o3_;
        chain[nchain++] = &o4_;
        chain[nchain++] = &o6_;
        chain[nchain++] = &word_;
        chain[nchain++] = &sp13_;
        chain[nchain++] = &sp24_;
        chain[nchain++] = &col_;
        chain[nchain++] = &tag_;
        if (HP_CM_WBI) chain[nchain++] = &wbi_;   // word bigram (off in Lite)
        chain[nchain++] = &brk_;
        chain[nchain++] = &sentst_;
        for (int i = 0; i < nchain; ++i) {
            chain[i]->predict(c0_, backoff, out);
            for (int j = 0; j < ContextModel::kOutputs; ++j) {
                mixer_.add(out[j]);
                exp_p_[n_exp_++] = squash(out[j]);
            }
            if (i < 4) backoff = chain[i]->last_p();
            else if (i == 4) backoff = o1_.last_p();
        }
#if HPD_RECORD
        {
            int rout[RecordModel::kOutputs];
            rec_.predict(c0_, o1_.last_p(), rout);
            for (int j = 0; j < RecordModel::kOutputs; ++j) {
                mixer_.add(rout[j]);
                exp_p_[n_exp_++] = squash(rout[j]);
            }
        }
#endif
        for (int i = 0; i < kMatchModels; ++i) {
            const int ms = match_[i].predict(c0_, bitpos_);
            mixer_.add(ms);
            exp_p_[n_exp_++] = squash(ms);
        }
        {
            const int ms = smatch_.predict(c0_, bitpos_);
            mixer_.add(ms);
            exp_p_[n_exp_++] = squash(ms);
        }
        for (int i = 0; i < kWordMatch; ++i) {
            const int ms = wmatch_[i].predict(c0_, bitpos_);
            mixer_.add(ms);
            exp_p_[n_exp_++] = squash(ms);
        }
        {
            int dout[kDiscovered];
            pool_.predict(c0_, o1_.last_p(), dout);
            for (int i = 0; i < pool_.outputs(); ++i) {
                mixer_.add(dout[i]);
                exp_p_[n_exp_++] = squash(dout[i]);
            }
        }
        sparse_ = o6_.sparsity();


        mixer_.set_ctx(kGateC0, c0_);
        mixer_.set_ctx(kGateAlpha, gria_.bucket());
        mixer_.set_ctx(kGatePrev, static_cast<int>(hist_ & 0xff));
        int mlen = 0;
        for (int i = 0; i < kMatchModels; ++i)
            if (match_[i].match_len() > mlen) mlen = match_[i].match_len();
        for (int i = 0; i < kWordMatch; ++i)
            if (wmatch_[i].match_len() > mlen) mlen = wmatch_[i].match_len();
        mixer_.set_ctx(kGateMatch, mlen > 31 ? 31 : mlen);
        mixer_.set_ctx(kGateHebb, hebb_.strength() > 15 ? 15 : hebb_.strength());
        mixer_.set_ctx(kGateEntropy, gria_.entropy_bucket());
        mixer_.set_ctx(kGateWiki, wiki_.state() + (wiki_.is_paragraph() << 4));
        mixer_.set_ctx(kGatePattern, cache_.cls());
        mixer_.set_ctx(kGateArgmax, argmax_bin(exp_p_, n_exp_));
        mixer_.set_ctx(kGateWordPos, streams_.word_len() > 15 ? 15 : streams_.word_len());
#if HPD_REC_GATE
        {
            // 0 = no record period; else the relative position inside the record.
            const int r = rec_.period();
            int g = 0;
            if (r) g = 1 + (r <= 63 ? rec_.column() : rec_.column() * 62 / r);
            mixer_.set_ctx(kGateRecord, g > 63 ? 63 : g);
        }
#endif
        mixer_.set_ctx2(c0_);
        int pr = mixer_.mix();

        for (int j = 0; j < mixer_.num_layer1(); ++j)
            hedge_.set(j, mixer_.layer1_p(j));
        const int ph = hedge_.mix();
        pr = (pr + ph) >> 1;
        mixed_p_ = pr;

        const int a = apm_c0_.refine(pr, c0_);
        const int b = apm_lex_.refine(pr, static_cast<int>(hist_ & 0xff) * 256 + c0_);
        const int g = apm_gria_.refine(pr, gria_.bucket() * 256 + c0_);
#if HPD_APM2
        {
            // hpd: order-2 (or 3) refinement, lpaq-style: hash of the last bytes xor c0.
            const std::uint32_t hh = static_cast<std::uint32_t>(
                ((hist_ & (HPD_APM2 == 3 ? 0xffffffull : 0xffffull)) * 0x9E3779B1ull) >> 16);
            const int o = apm_o2_.refine(pr, static_cast<int>((hh ^ static_cast<std::uint32_t>(c0_)) & 0xffff));
            pr_final_ = clamp_int((HPD_WA2 * a + HPD_WB2 * b + HPD_WG2 * g + HPD_WO2 * o) >> 3, 1, 4094);
        }
#else
        pr_final_ = clamp_int((HP_W0 * pr + HP_WA * a + HP_WB * b + HP_WG * g) >> 3, 1, 4094);
#endif
        return pr_final_;
    }

    void update(int y) {
        gria_.account_bit(y ? pr_final_ : 4096 - pr_final_);

        mixer_.update(y);
        hedge_.update(y, gria_.switch_rate_q16());
        apm_c0_.update(y);
        apm_lex_.update(y);
        apm_gria_.update(y);
#if HPD_APM2
        apm_o2_.update(y);
#endif

        counter_update(bias_[c0_], y, 1023);
        const int ens = pr_final_;
        o1_.update(y, ens); o2_.update(y, ens); o3_.update(y, ens);
        o4_.update(y, ens); o6_.update(y, ens); word_.update(y, ens);
        if (HP_CM_SP) { sp13_.update(y, ens); sp24_.update(y, ens); }
        if (HP_CM_COL) col_.update(y, ens);
        if (HP_CM_TAG) tag_.update(y, ens);
        if (HP_CM_WBI) wbi_.update(y, ens);
        brk_.update(y, ens);
        sentst_.update(y, ens);
#if HPD_RECORD
        rec_.update(y, ens);
#endif
        for (int i = 0; i < kMatchModels; ++i) match_[i].update(y);
        smatch_.update(y);
        for (int i = 0; i < kWordMatch; ++i) wmatch_[i].update(y);
        hebb_.update(y);
        pool_.update(y, y ? (4096 - pr_final_) >> 4 : pr_final_ >> 4);

        c0_ = (c0_ << 1) | y;
        ++bitpos_;
#if HPD_BUCKET
        if (bitpos_ == 4) {
            // Second nibble: issue every context model's cache-line load now,
            // so the predict() calls for bit 4 find them in flight or in cache.
            ContextModel* cms[] = {&o1_, &o2_, &o3_, &o4_, &o6_, &word_, &sp13_, &sp24_,
                                   &col_, &tag_, &brk_, &sentst_, &wbi_};
            for (ContextModel* m : cms) m->prefetch_nibble(c0_);
            pool_.prefetch_nibble(c0_);
#if HPD_RECORD
            rec_.prefetch_nibble(c0_);
#endif
        }
#endif
        if (bitpos_ == 8) {
            const int byte = c0_ & 0xff;
            c0_ = 1;
            bitpos_ = 0;
            end_of_byte(byte);
        }
    }

    const GriaGate& gria() const { return gria_; }
    int discovery_replacements() const { return pool_.replaced(); }
    int cache_hits() const { return cache_.hits(); }
    int cache_lookups() const { return cache_.lookups(); }
    const PatternCache& patterns() const { return cache_; }

    int expert_count() const { return n_exp_; }
    int expert_p(int i) const { return exp_p_[i]; }
    int mixed_p() const { return mixed_p_; }
    int sparse_fraction() const { return sparse_; }

 private:
    static vec<int> gate_sizes() {
        vec<int> s = {256, GriaGate::kBuckets, 256, 32,
                              GriaGate::kEntBuckets, 16};
        s.push_back(32);   // wiki state × isParagraph
        s.push_back(PatternCache::kNClass);
        s.push_back(16);
        s.push_back(16);
#if HPD_REC_GATE
        s.push_back(64);
#endif
        return s;
    }

    static vec<int> gate_rates(int base) {
        // Spread inspired by fx2-cmix's 0.0003–0.005, mapped onto hp's
        // integer lr where 2 is the historical shared default.
        (void)base;
        vec<int> r = {2, 3, 2, 4, 3, 4};
        r.push_back(3);
        r.push_back(3);
        r.push_back(3);
        r.push_back(3);
#if HPD_REC_GATE
        r.push_back(3);
#endif
        return r;
    }

    static int backoff_kt_(const Counter& c) {
        return counter_predict_p(c);
    }


    std::uint32_t h2(std::uint64_t salt, std::uint64_t key) {
        return cache_.hash_memo(salt, key);
    }

    void end_of_byte(int byte) {
        hist_ = (hist_ << 8) | static_cast<std::uint64_t>(byte);
#if HPD_RECORD
        rec_.push(byte);
#endif

        const bool alnum = (byte >= 'a' && byte <= 'z') ||
                           (byte >= 'A' && byte <= 'Z') ||
                           (byte >= '0' && byte <= '9');
        const int at_boundary = (!alnum && word_hash_ != 0) ? 1 : 0;
        const bool letter = (byte >= 'a' && byte <= 'z') ||
                            (byte >= 'A' && byte <= 'Z');
        if (alnum) {
            word_hash_ = mix64(word_hash_ * 0x100000001B3ull +
                               static_cast<std::uint64_t>(byte | 0x20));
        } else {
            word_hash_ = 0;
        }
        if (letter) {
            letter_hash_ = mix64(letter_hash_ * 0x100000001B3ull +
                                 static_cast<std::uint64_t>(byte | 0x20));
        } else {
            letter_hash_ = 0;
        }

        if (byte == '\n') {
            for (int i = 0; i < kLineMax; ++i)
                prev_line_[i] = (i < col_pos_) ? cur_line_[i] : 0;
            col_pos_ = 0;
        } else {
            if (col_pos_ < kLineMax) cur_line_[col_pos_] = static_cast<std::uint8_t>(byte);
            if (col_pos_ < kLineMax - 1) ++col_pos_;
        }

        wiki_.push(byte);

        brackets_.push(byte);
        streams_.push(byte, alnum);

        if (!alnum && word_hash_prev_ != 0) {
            hebb_.potentiate(prev_word_, word_hash_prev_);
            prev_word_ = word_hash_prev_;
            word_ring_[3] = word_ring_[2];
            word_ring_[2] = word_ring_[1];
            word_ring_[1] = word_ring_[0];
            word_ring_[0] = prev_word_;
        }
        word_hash_prev_ = word_hash_;
        hebb_.set_context(prev_word_);
        hist2_ = (hist2_ << 8) | ((hist_ >> 56) & 0xffull);
        pool_.end_byte();
        pool_.set_contexts(hist_, hist2_);

        for (int i = 0; i < kMatchModels; ++i) match_[i].push_byte(byte, hist_);
        {
            std::uint64_t sh = 0;
            for (int i = 0; i < 4; ++i)
                sh = (sh << 8) | ((hist_ >> (16 * i)) & 0xffull);
            smatch_.push_byte(byte, sh);
        }
        // fx2 keys: {0} current, {1,3} current-alt + word-before-prev, {7,2} letters + last
        const std::uint64_t w0 = word_hash_ ? word_hash_ : word_ring_[0];
        const std::uint64_t w13 = mix64(word_hash_ * 263ull + word_ring_[1]);
        const std::uint64_t w72 = mix64(letter_hash_ * 997ull + word_ring_[0]);
        std::uint64_t whist[5] = {w0, w13, w72, 0, 0};
        int nw = 3;
        (void)nw;
        for (int i = 0; i < kWordMatch; ++i)
            wmatch_[i].push_byte(byte, whist[i], at_boundary);

        cache_.observe_byte(byte, wiki_.state(), wiki_.in_table(),
                            streams_.first_class());

        gria_.account_byte(byte);
        set_byte_contexts();
    }

    void set_byte_contexts() {
        const int col = (col_pos_ < kLineMax) ? col_pos_ : kLineMax - 1;
        col_.set_context(h2(21, (static_cast<std::uint64_t>(prev_line_[col]) << 16) |
                                    static_cast<std::uint64_t>(col & 63)));
        tag_.set_context(h2(22, wiki_.context_key()));
        wbi_.set_context(h2(23, prev_word_ * 0x9E3779B97F4A7C15ull + word_hash_));

        o1_.set_context(h2(1, hist_ & 0xffull));
        o2_.set_context(h2(2, hist_ & 0xffffull));
        o3_.set_context(h2(3, hist_ & 0xffffffull));
        o4_.set_context(h2(4, hist_ & 0xffffffffull));
        o6_.set_context(h2(6, hist_ & 0xffffffffffffull));
        word_.set_context(h2(7, word_hash_));
        sp13_.set_context(h2(8, ((hist_ >> 0) & 0xffull) |
                                   (((hist_ >> 16) & 0xffull) << 8)));
        sp24_.set_context(h2(9, ((hist_ >> 8) & 0xffull) |
                                   (((hist_ >> 24) & 0xffull) << 8)));
        {
            std::uint64_t bk = brackets_.context_key();
            bk += static_cast<std::uint64_t>(brackets_.quote()) << 24;
            brk_.set_context(h2(26, bk));
        }
        sentst_.set_context(h2(34, streams_.stream(3) * 83ull + (hist_ & 0xffull)));
    }

    Config cfg_;
    int slot_cap_;                          // must follow cfg_ and precede every model
    shared<HistoryBuf> histbuf_;   // declared before every model that uses it
#if HPD_RECORD
    RecordModel rec_;
#endif
    ContextModel o1_, o2_, o3_, o4_, o6_, word_;
    ContextModel col_, tag_, wbi_;
    ContextModel sp13_, sp24_;
    ContextModel brk_;
    ContextModel sentst_;
    MatchModel match_[kMatchModels];
    MatchModel smatch_;
    WordMatchModel wmatch_[3 + (HP_WMATCH_4 ? 1 : 0) + (HP_WMATCH_5 ? 1 : 0)];
    HebbianModel hebb_;
    DiscoveryPool pool_;
    MixerNet mixer_;
    APM apm_c0_, apm_lex_, apm_gria_;
#if HPD_APM2
    APM apm_o2_;
#endif
    Hedge hedge_;
    vec<Counter> bias_;
    GriaGate gria_;
    WikiMachine wiki_;
    WordStreams streams_;
    BracketMachine brackets_;
    PatternCache cache_;

    std::uint64_t hist_ = 0;
    std::uint64_t word_hash_ = 0;
    std::uint64_t letter_hash_ = 0;
    std::uint64_t hist2_ = 0;
    static constexpr int kLineMax = 256;
    std::uint8_t prev_line_[kLineMax] = {0};
    std::uint8_t cur_line_[kLineMax] = {0};
    int col_pos_ = 0;
    int tag_depth_ = 0;
    int in_tag_ = 0;
    std::uint64_t tag_name_ = 0;
    std::uint64_t prev_word_ = 0;
    std::uint64_t word_hash_prev_ = 0;
    std::uint64_t word_ring_[4] = {0, 0, 0, 0};
    int c0_ = 1;
    int bitpos_ = 0;
    int pr_final_ = 2048;
    int exp_p_[kNumExperts + 8] = {0};
    int n_exp_ = 0;
    int mixed_p_ = 2048;
    int sparse_ = 0;
};

}  // namespace hp
