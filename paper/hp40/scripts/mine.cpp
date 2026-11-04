// mine.cpp — expert-level data mining for hp (diagnostic only; floats allowed here).
// usage: mine <in> <out_prefix> [mem=22] [stride=29] [maxrec=300000] [block=65536]
// Writes <out_prefix>.json (full-pass stats over every bit) and <out_prefix>.i16
// (sampled records: n expert stretches, final stretch, bit).
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include "hp/predictor.hpp"

static double cost(int p12, int y) {  // bits to code y given P(1)=p12/4096
    double p = p12 / 4096.0;
    if (p < 1.0 / 4096) p = 1.0 / 4096;
    if (p > 4095.0 / 4096) p = 4095.0 / 4096;
    return -std::log2(y ? p : 1 - p);
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: mine in out_prefix [mem] [stride] [maxrec] [block]\n"); return 2; }
    const int mem = argc > 3 ? std::atoi(argv[3]) : 22;
    const int stride = argc > 4 ? std::atoi(argv[4]) : 29;
    const long maxrec = argc > 5 ? std::atol(argv[5]) : 300000;
    const long block = argc > 6 ? std::atol(argv[6]) : 65536;
    std::FILE* in = std::fopen(argv[1], "rb");
    if (!in) { std::perror(argv[1]); return 1; }
    std::string pre = argv[2];
    std::FILE* rec = std::fopen((pre + ".i16").c_str(), "wb");

    hp::Config cfg;
    cfg.table_bits = mem;
    cfg.normalize();
    hp::Predictor pred(cfg);

    std::vector<double> eloss;           // per-expert total bits
    std::vector<long> ebest;             // bits where expert e had the lowest cost
    std::vector<double> bitpos_loss(8, 0.0);
    std::vector<double> blk_loss;        // final loss per block
    double floss = 0, oracle = 0;
    long nbits = 0, seen = 0, kept = 0, nbytes = 0;
    std::vector<std::int16_t> r;
    int c;
    while ((c = std::fgetc(in)) != EOF) {
        for (int i = 7; i >= 0; --i) {
            const int p = pred.predict();
            const int y = (c >> i) & 1;
            const int n = pred.expert_count();
            if ((int)eloss.size() < n) { eloss.resize(n, 0.0); ebest.resize(n, 0); }
            double best = 1e9; int bi = 0;
            for (int e = 0; e < n; ++e) {
                const double ce = cost(hp::clamp_int(pred.expert_p(e), 1, 4095), y);
                eloss[e] += ce;
                if (ce < best) { best = ce; bi = e; }
            }
            ebest[bi]++;
            oracle += best;
            const double cf = cost(p, y);
            floss += cf;
            bitpos_loss[7 - i] += cf;
            const long b = nbytes / block;
            if ((long)blk_loss.size() <= b) blk_loss.resize(b + 1, 0.0);
            blk_loss[b] += cf;
            if (rec && kept < maxrec && (seen % stride) == 0) {
                r.clear();
                for (int e = 0; e < n; ++e)
                    r.push_back((std::int16_t)hp::stretch(hp::clamp_int(pred.expert_p(e), 1, 4094)));
                r.push_back((std::int16_t)hp::stretch(hp::clamp_int(p, 1, 4094)));
                r.push_back((std::int16_t)y);
                std::fwrite(r.data(), 2, r.size(), rec);
                ++kept;
            }
            ++seen; ++nbits;
            pred.update(y);
        }
        ++nbytes;
    }
    if (rec) std::fclose(rec);
    std::FILE* js = std::fopen((pre + ".json").c_str(), "w");
    std::fprintf(js, "{\"file\":\"%s\",\"mem\":%d,\"bytes\":%ld,\"bits\":%ld,\"n_experts\":%zu,\"records\":%ld,\"stride\":%d,\n",
                 argv[1], mem, nbytes, nbits, eloss.size(), kept, stride);
    std::fprintf(js, "\"final_bits\":%.3f,\"oracle_bits\":%.3f,\n\"expert_bits\":[", floss, oracle);
    for (size_t e = 0; e < eloss.size(); ++e) std::fprintf(js, "%s%.3f", e ? "," : "", eloss[e]);
    std::fprintf(js, "],\n\"expert_best\":[");
    for (size_t e = 0; e < ebest.size(); ++e) std::fprintf(js, "%s%ld", e ? "," : "", ebest[e]);
    std::fprintf(js, "],\n\"bitpos_bits\":[");
    for (int k = 0; k < 8; ++k) std::fprintf(js, "%s%.3f", k ? "," : "", bitpos_loss[k]);
    std::fprintf(js, "],\n\"block\":%ld,\"block_bits\":[", block);
    for (size_t k = 0; k < blk_loss.size(); ++k) std::fprintf(js, "%s%.2f", k ? "," : "", blk_loss[k]);
    std::fprintf(js, "]}\n");
    std::fclose(js);
    std::fprintf(stderr, "mine: %ld bytes, %zu experts, final %.4f bpc, oracle %.4f bpc\n",
                 nbytes, eloss.size(), floss / nbytes, oracle / nbytes);
    return 0;
}
