#pragma once
//
// hp/warp_ops.hpp — Predictor::predict / update run by one CUDA warp (hpd-gpu, P3).
//
// The 32 lanes of a warp share one shard. Work that touches disjoint state runs
// on different lanes: every context model's table lookup and estimate (13 main,
// 3 record, n discovery), the 11 match models, the 11 layer-1 mixer dot
// products (split over all lanes, exact int64 sums), and all model / mixer /
// APM updates. Lane 0 does the short serial parts (mixer gates, layer 2, hedge,
// APMs, end-of-byte bookkeeping). The result is bit-identical to
// Predictor::predict / update: same values, same order wherever order matters.
//
// Every lane must call predict / update together. Device code only.

#include "hp/predictor.hpp"

#ifndef HPD_WARP_UNROLL
#define HPD_WARP_UNROLL 0   // read all mixer rows in one pass (measured: slower on RTX 3090)
#endif

namespace hp {

#if defined(HPD_WARP_PROF)
// Cycle counts per phase, summed by lane 0 of every warp (profiling build only).
__device__ unsigned long long hp_prof[16];
#define HP_PROF_T0 long long _pt = clock64();
#define HP_PROF(i) do { if (lane == 0) { const long long _n = clock64(); prof_acc()[i] += _n - _pt; _pt = _n; } } while (0)
#else
#define HP_PROF_T0
#define HP_PROF(i) do { } while (0)
#endif

struct WarpOps {
    static constexpr unsigned kFull = 0xffffffffu;
    static constexpr int kMaxL1 = 12;   // layer-1 mixers held in registers (hpd has 11)
#if defined(HPD_WARP_PROF)
    __device__ static unsigned long long* prof_acc() {
        static __shared__ unsigned long long acc[16];
        return acc;
    }
#endif

    // The context-model chain in Predictor::predict order.
    __device__ static int chain(Predictor& P, ContextModel** c) {
        int n = 0;
        c[n++] = &P.o1_; c[n++] = &P.o2_; c[n++] = &P.o3_; c[n++] = &P.o4_; c[n++] = &P.o6_;
        c[n++] = &P.word_; c[n++] = &P.sp13_; c[n++] = &P.sp24_; c[n++] = &P.col_; c[n++] = &P.tag_;
        if (HP_CM_WBI) c[n++] = &P.wbi_;
        c[n++] = &P.brk_; c[n++] = &P.sentst_;
        return n;
    }

    struct Layout {
        ContextModel* ch[16];
        int nchain, nrec, npool, nmatch, ncm;
        int base_rec, base_match, base_pool, total;   // mixer input positions
    };

    __device__ static void layout(Predictor& P, Layout& L) {
        L.nchain = chain(P, L.ch);
#if HPD_RECORD
        L.nrec = RecordModel::kModels;
#else
        L.nrec = 0;
#endif
        L.npool = P.pool_.n_;
        L.nmatch = Predictor::kMatchModels + 1 + Predictor::kWordMatch;
        L.ncm = L.nchain + L.nrec + L.npool;
        L.base_rec = 1 + 2 * L.nchain;
        L.base_match = L.base_rec + 2 * L.nrec;
        L.base_pool = L.base_match + L.nmatch;
        L.total = L.base_pool + 2 * L.npool;
    }

    __device__ static ContextModel* cm_task(Predictor& P, const Layout& L, int t) {
        if (t < L.nchain) return L.ch[t];
#if HPD_RECORD
        if (t < L.nchain + L.nrec) return &P.rec_.cm_[t - L.nchain];
#endif
        return &P.pool_.models_[t - L.nchain - L.nrec];
    }
    // Mixer input position of CM task t's first output.
    __device__ static int cm_pos(const Layout& L, int t) {
        if (t < L.nchain) return 1 + 2 * t;
        if (t < L.nchain + L.nrec) return L.base_rec + 2 * (t - L.nchain);
        return L.base_pool + 2 * (t - L.nchain - L.nrec);
    }

    __device__ static void put(Predictor& P, int pos, int stretched) {
        if (pos < P.mixer_.n_) P.mixer_.st_[pos] = stretched;
        P.exp_p_[pos - 1] = squash(stretched);
    }

    __device__ static int predict(Predictor& P, int lane) {
        MixerNet& M = P.mixer_;
        Layout L;
        layout(P, L);
        const int c0 = P.c0_, bitpos = P.bitpos_;
        const int bias_p = counter_predict_p(P.bias_[c0]);
        HP_PROF_T0

        // 1. Table lookups and indirect estimates; match models.
        for (int t = lane; t < L.ncm + L.nmatch; t += 32) {
            if (t < L.ncm) {
                cm_task(P, L, t)->predict_ind(c0);
            } else {
                const int m = t - L.ncm;
                int ms;
                if (m < Predictor::kMatchModels) ms = P.match_[m].predict(c0, bitpos);
                else if (m == Predictor::kMatchModels) ms = P.smatch_.predict(c0, bitpos);
                else ms = P.wmatch_[m - Predictor::kMatchModels - 1].predict(c0, bitpos);
                put(P, L.base_match + m, ms);
            }
        }
        __syncwarp();
        HP_PROF(0);
        // 2. PY estimates: backoff is the bias, then the previous order (o1..o4),
        //    then o1 for everything after o6, the record and the discovery models.
        for (int t = lane; t < L.ncm; t += 32) {
            int backoff;
            if (t == 0) backoff = bias_p;
            else if (t <= 4) backoff = L.ch[t - 1]->last_p();
            else backoff = P.o1_.last_p();
            int out[ContextModel::kOutputs];
            ContextModel* cm = cm_task(P, L, t);
            cm->predict_py(backoff, out);
            const int pos = cm_pos(L, t);
            put(P, pos, out[0]);
            put(P, pos + 1, out[1]);
            if (t >= L.nchain + L.nrec) P.pool_.p_[t - L.nchain - L.nrec] = cm->last_p();
        }
        if (lane == 0) {
            M.st_[0] = counter_predict(P.bias_[c0]);
            M.m_ = L.total < M.n_ ? L.total : M.n_;
        }
        __syncwarp();
        HP_PROF(1);

        // 3. Gates (lane 0), exactly as Predictor::predict.
        if (lane == 0) {
            P.n_exp_ = L.total - 1;   // exp_p_[0] holds o1's first output, as on the CPU
            P.sparse_ = P.o6_.sparsity();
            M.set_ctx(Predictor::kGateC0, c0);
            M.set_ctx(Predictor::kGateAlpha, P.gria_.bucket());
            M.set_ctx(Predictor::kGatePrev, static_cast<int>(P.hist_ & 0xff));
            int mlen = 0;
            for (int i = 0; i < Predictor::kMatchModels; ++i)
                if (P.match_[i].match_len() > mlen) mlen = P.match_[i].match_len();
            for (int i = 0; i < Predictor::kWordMatch; ++i)
                if (P.wmatch_[i].match_len() > mlen) mlen = P.wmatch_[i].match_len();
            M.set_ctx(Predictor::kGateMatch, mlen > 31 ? 31 : mlen);
            M.set_ctx(Predictor::kGateHebb, P.hebb_.strength() > 15 ? 15 : P.hebb_.strength());
            M.set_ctx(Predictor::kGateEntropy, P.gria_.entropy_bucket());
            M.set_ctx(Predictor::kGateWiki, P.wiki_.state() + (P.wiki_.is_paragraph() << 4));
            M.set_ctx(Predictor::kGatePattern, P.cache_.cls());
            M.set_ctx(Predictor::kGateArgmax, argmax_bin(P.exp_p_, P.n_exp_));
            M.set_ctx(Predictor::kGateWordPos, P.streams_.word_len() > 15 ? 15 : P.streams_.word_len());
#if HPD_REC_GATE
            {
                const int r = P.rec_.period();
                int g = 0;
                if (r) g = 1 + (r <= 63 ? P.rec_.column() : P.rec_.column() * 62 / r);
                M.set_ctx(Predictor::kGateRecord, g > 63 ? 63 : g);
            }
#endif
            M.set_ctx2(c0);
        }
        __syncwarp();
        HP_PROF(2);

        // 4. Layer-1 dot products over all lanes (int64 sums are exact in any order).
        //    All rows are read in one pass so their memory latencies overlap.
        const int m = M.m_, n = M.n_, K = M.k_;
        if (HPD_WARP_UNROLL && K <= kMaxL1) {
            const std::int32_t* wr[kMaxL1];
            std::int64_t s[kMaxL1];
#pragma unroll
            for (int j = 0; j < kMaxL1; ++j) {
                s[j] = 0;
                wr[j] = j < K ? M.w_[j].data() + static_cast<std::size_t>(M.ctx_[j]) * n : nullptr;
            }
            for (int i = lane; i < m; i += 32) {
                const std::int64_t x = M.st_[i];
#pragma unroll
                for (int j = 0; j < kMaxL1; ++j)
                    if (j < K) s[j] += static_cast<std::int64_t>(wr[j][i]) * x;
            }
#pragma unroll
            for (int j = 0; j < kMaxL1; ++j)
                for (int off = 16; off; off >>= 1) s[j] += __shfl_xor_sync(kFull, s[j], off);
            if (lane == 0)
                for (int j = 0; j < K; ++j) {
                    M.dot_[j] = clamp_int(static_cast<int>(s[j] >> 16), -2047, 2047);
                    M.pr_[j] = squash(M.dot_[j]);
                }
        } else {
            for (int j = 0; j < K; ++j) {
                const std::int32_t* w = &M.w_[j][static_cast<std::size_t>(M.ctx_[j]) * n];
                std::int64_t s = 0;
                for (int i = lane; i < m; i += 32) s += static_cast<std::int64_t>(w[i]) * M.st_[i];
                for (int off = 16; off; off >>= 1) s += __shfl_xor_sync(kFull, s, off);
                if (lane == 0) {
                    M.dot_[j] = clamp_int(static_cast<int>(s >> 16), -2047, 2047);
                    M.pr_[j] = squash(M.dot_[j]);
                }
            }
        }
        __syncwarp();
        HP_PROF(3);

        // 5. Layer 2, hedge, APMs (lane 0).
        if (lane == 0) {
            const std::int32_t* v = &M.v_[static_cast<std::size_t>(M.ctx2_) * M.k_];
            std::int64_t sum = 0;
            for (int j = 0; j < M.k_; ++j) sum += static_cast<std::int64_t>(v[j]) * M.dot_[j];
            M.final_dot_ = clamp_int(static_cast<int>(sum >> 16), -2047, 2047);
            M.final_pr_ = squash(M.final_dot_);
            int pr = M.final_pr_;
            for (int j = 0; j < M.k_; ++j) P.hedge_.set(j, M.pr_[j]);
            const int ph = P.hedge_.mix();
            pr = (pr + ph) >> 1;
            P.mixed_p_ = pr;
            const int a = P.apm_c0_.refine(pr, c0);
            const int b = P.apm_lex_.refine(pr, static_cast<int>(P.hist_ & 0xff) * 256 + c0);
            const int g = P.apm_gria_.refine(pr, P.gria_.bucket() * 256 + c0);
#if HPD_APM2
            {
                const std::uint32_t hh = static_cast<std::uint32_t>(
                    ((P.hist_ & (HPD_APM2 == 3 ? 0xffffffull : 0xffffull)) * 0x9E3779B1ull) >> 16);
                const int o = P.apm_o2_.refine(pr, static_cast<int>((hh ^ static_cast<std::uint32_t>(c0)) & 0xffff));
                P.pr_final_ = clamp_int((HPD_WA2 * a + HPD_WB2 * b + HPD_WG2 * g + HPD_WO2 * o) >> 3, 1, 4094);
            }
#else
            P.pr_final_ = clamp_int((HP_W0 * pr + HP_WA * a + HP_WB * b + HP_WG * g) >> 3, 1, 4094);
#endif
        }
        __syncwarp();
        HP_PROF(4);
        return P.pr_final_;
    }

    // Hedge::update with one expert per lane; the weight sum is an exact
    // uint64 warp reduction, so every division sees the same total.
    __device__ static void hedge_update(Hedge& H, int lane, int y, std::uint32_t sigma_q16) {
        const int n = H.n_;
        if (n > 32) {
            if (lane == 0) H.update(y, sigma_q16);
            __syncwarp();
            return;
        }
        std::uint32_t w = 0;
        if (lane < n) {
            const int p = H.p_[lane];
            const int pa = y ? p : 4096 - p;
            const std::uint32_t loss_q16 = (12u << 16) - log2_q16(static_cast<std::uint32_t>(pa < 1 ? 1 : pa));
            const std::uint32_t decay = ((loss_q16 >> 8) * HP_HEDGE_ETA) >> 8;
            const std::uint32_t keepf = 65536u - (decay > 8192u ? 8192u : decay);
            w = static_cast<std::uint32_t>((static_cast<std::uint64_t>(H.w_[lane]) * keepf) >> 16);
            if (w < 16) w = 16;
        }
        std::uint64_t wsum = lane < n ? w : 0;
        for (int off = 16; off; off >>= 1) wsum += __shfl_xor_sync(kFull, wsum, off);
        if (lane < n) {
            if (wsum > 0) w = static_cast<std::uint32_t>((static_cast<std::uint64_t>(w) << 16) / wsum);
            if (sigma_q16 > 0) {
                const std::uint32_t unif = (1u << 16) / static_cast<std::uint32_t>(n);
                const std::uint64_t keep = (static_cast<std::uint64_t>(w) * (65536u - sigma_q16)) >> 16;
                const std::uint64_t share = (static_cast<std::uint64_t>(unif) * sigma_q16) >> 16;
                w = static_cast<std::uint32_t>(keep + share);
            }
            H.w_[lane] = w;
        }
        __syncwarp();
    }

    __device__ static void update(Predictor& P, int lane, int y) {
        MixerNet& M = P.mixer_;
        Layout L;
        layout(P, L);
        const int prf = P.pr_final_;
        const int c0 = P.c0_;
        HP_PROF_T0

        // Serial pieces, each on its own lane: gria + hedge (hedge reads gria after
        // its update), the three APMs, the bias counter.
        if (lane == 0) {
            P.gria_.account_bit(y ? prf : 4096 - prf);
        } else if (lane == 1) {
            P.apm_c0_.update(y);
        } else if (lane == 2) {
            P.apm_lex_.update(y);
        } else if (lane == 3) {
            P.apm_gria_.update(y);
#if HPD_APM2
        } else if (lane == 5) {
            P.apm_o2_.update(y);
#endif
        } else if (lane == 4) {
            counter_update(P.bias_[c0], y, 1023);
        }

        __syncwarp();
        hedge_update(P.hedge_, lane, y, P.gria_.switch_rate_q16());
        HP_PROF(5);
        // Mixer (MixerNet::update), weights split over lanes.
        {
            const int t = y << 12;
            const int err2 = t - M.final_pr_;
            const int ae = err2 < 0 ? -err2 : err2;
            if (ae >= HP_MIXER_SKIP) {
                std::int32_t* v = &M.v_[static_cast<std::size_t>(M.ctx2_) * M.k_];
                for (int j = lane; j < M.k_; j += 32) {
                    const std::int32_t dv = static_cast<std::int32_t>(
                        (static_cast<std::int64_t>(M.dot_[j]) * err2 * M.lr_) >> 14);
                    v[j] = clamp_int(v[j] + dv, -(1 << 22), (1 << 22));
                }
                const int m = M.m_, K = M.k_;
                if (HPD_WARP_UNROLL && K <= kMaxL1) {
                    std::int32_t* wr[kMaxL1];
                    std::int32_t mm[kMaxL1];
#pragma unroll
                    for (int j = 0; j < kMaxL1; ++j) {
                        wr[j] = j < K ? M.w_[j].data() + static_cast<std::size_t>(M.ctx_[j]) * M.n_ : nullptr;
                        mm[j] = j < K ? (t - M.pr_[j]) * M.lr1_[static_cast<std::size_t>(j)] : 0;
                    }
                    for (int i = lane; i < m; i += 32) {
                        const std::int32_t x = M.st_[i];
#pragma unroll
                        for (int j = 0; j < kMaxL1; ++j) {
                            if (j < K) {
                                std::int32_t nv = wr[j][i] + ((x * mm[j]) >> 14);
                                nv = nv < -(1 << 22) ? -(1 << 22) : (nv > (1 << 22) ? (1 << 22) : nv);
                                wr[j][i] = nv;
                            }
                        }
                    }
                } else {
                    for (int j = 0; j < K; ++j) {
                        std::int32_t* w = &M.w_[j][static_cast<std::size_t>(M.ctx_[j]) * M.n_];
                        const std::int32_t mm = (t - M.pr_[j]) * M.lr1_[static_cast<std::size_t>(j)];
                        for (int i = lane; i < m; i += 32) {
                            std::int32_t nv = w[i] + ((M.st_[i] * mm) >> 14);
                            nv = nv < -(1 << 22) ? -(1 << 22) : (nv > (1 << 22) ? (1 << 22) : nv);
                            w[i] = nv;
                        }
                    }
                }
            }
        }

        HP_PROF(6);
        // Model updates: every model owns its state, so one lane each.
        const int ens = prf;
        const int ens_err = y ? (4096 - prf) >> 4 : prf >> 4;
        for (int t = lane; t < L.ncm + L.nmatch + 1; t += 32) {
            if (t < L.nchain) {
                ContextModel* cm = L.ch[t];
                bool on = true;
                if (cm == &P.sp13_ || cm == &P.sp24_) on = HP_CM_SP;
                else if (cm == &P.col_) on = HP_CM_COL;
                else if (cm == &P.tag_) on = HP_CM_TAG;
                else if (cm == &P.wbi_) on = HP_CM_WBI;
                if (on) cm->update(y, ens);
            } else if (t < L.nchain + L.nrec) {
#if HPD_RECORD
                P.rec_.cm_[t - L.nchain].update(y, ens);
#endif
            } else if (t < L.ncm) {
                const int i = t - L.nchain - L.nrec;
                DiscoveryPool& D = P.pool_;
                D.models_[i].update(y);
                const int pa = y ? D.p_[i] : 4096 - D.p_[i];
                const std::uint32_t cost = (12u << 16) - log2_q16(static_cast<std::uint32_t>(pa < 1 ? 1 : pa));
                D.loss_[i] += (static_cast<std::uint64_t>(cost) * static_cast<std::uint32_t>(ens_err + 64)) >> 8;
            } else if (t < L.ncm + L.nmatch) {
                const int mi = t - L.ncm;
                if (mi < Predictor::kMatchModels) P.match_[mi].update(y);
                else if (mi == Predictor::kMatchModels) P.smatch_.update(y);
                else P.wmatch_[mi - Predictor::kMatchModels - 1].update(y);
            } else {
                P.hebb_.update(y);
            }
        }
        __syncwarp();
        if (lane == 0) {
            P.c0_ = (P.c0_ << 1) | y;
            ++P.bitpos_;
        }
        __syncwarp();
        HP_PROF(7);
        const int bp = P.bitpos_;   // read once: lane 0 resets it below
#if HPD_BUCKET
        if (bp == 4) {
            // Second nibble: every model locates its bucket (Predictor::update).
            ContextModel* cms[] = {&P.o1_, &P.o2_, &P.o3_, &P.o4_, &P.o6_, &P.word_, &P.sp13_, &P.sp24_,
                                   &P.col_, &P.tag_, &P.brk_, &P.sentst_, &P.wbi_};
            const int nfix = 13;
            const int n = nfix + P.pool_.n_ + L.nrec;
            const int c = P.c0_;
            for (int t = lane; t < n; t += 32) {
                if (t < nfix) cms[t]->prefetch_nibble(c);
                else if (t < nfix + P.pool_.n_) P.pool_.models_[t - nfix].prefetch_nibble(c);
#if HPD_RECORD
                else P.rec_.cm_[t - nfix - P.pool_.n_].prefetch_nibble(c);
#endif
            }
            __syncwarp();
            HP_PROF(8);
        }
#endif
        if (bp == 8) {
            if (lane == 0) {
                const int byte = P.c0_ & 0xff;
                P.c0_ = 1;
                P.bitpos_ = 0;
                P.end_of_byte(byte);
            }
            __syncwarp();
            HP_PROF(9);
        }
    }
};

}  // namespace hp
