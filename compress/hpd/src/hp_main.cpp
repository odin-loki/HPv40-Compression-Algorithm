// hp — Cypha Hutter Prize core.
//
//   hp c [opts] <in> <out>     compress
//   hp d       <in> <out>      decompress
//
// Options:
//   --no-gria     disable GRIA alpha gating (ablation)
//   --no-dict     disable dictionary preprocessing (ablation)
//   --mem N       table bits per model (default 22)
//   --lr N        mixer learning rate (default 2)
//   --profile     print the CTW-style redundancy decomposition
//
// The archive header carries every flag that affects modelling, so `hp d`
// needs no options. The decompressor is standalone, as the rules require.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>

#include "hp/coder.hpp"
#include "hp/dict.hpp"
#include "hp/predictor.hpp"
#include "hp/profile.hpp"

namespace {

constexpr char kMagic[4] = {'C', 'Y', 'H', 'P'};
constexpr int kVersion = 4;

void put32(std::FILE* f, std::uint32_t v) {
    for (int i = 3; i >= 0; --i) std::fputc((v >> (i * 8)) & 0xff, f);
}
std::uint32_t get32(std::FILE* f) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v = (v << 8) | static_cast<std::uint32_t>(std::fgetc(f) & 0xff);
    return v;
}
void put64(std::FILE* f, std::uint64_t v) {
    put32(f, static_cast<std::uint32_t>(v >> 32));
    put32(f, static_cast<std::uint32_t>(v & 0xffffffffu));
}
std::uint64_t get64(std::FILE* f) {
    const std::uint64_t hi = get32(f);
    const std::uint64_t lo = get32(f);
    return (hi << 32) | lo;
}

bool read_all(const char* path, std::vector<std::uint8_t>& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) { std::perror(path); return false; }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<std::size_t>(n));
    if (n > 0 && std::fread(out.data(), 1, static_cast<std::size_t>(n), f) !=
                     static_cast<std::size_t>(n)) {
        std::fclose(f);
        return false;
    }
    std::fclose(f);
    return true;
}

int usage() {
    std::fprintf(stderr,
        "usage: hp c|d [--no-gria] [--dict] [--mem N] [--lr N] [--profile] "
        "<in> <out>\n");
    return 2;
}

// Code one buffer through the predictor + coder. Shared so the profiler and
// the real path can never diverge.
void encode_buffer(const std::vector<std::uint8_t>& buf, hp::Config cfg,
                   std::FILE* out, hp::Profiler* prof) {
    hp::Predictor pred(cfg);
    hp::Encoder enc(out);
    for (std::size_t k = 0; k < buf.size(); ++k) {
        const int c = buf[k];
        for (int i = 7; i >= 0; --i) {
            const int p = pred.predict();
            const int bit = (c >> i) & 1;
            if (prof) prof->account(p, bit, pred);
            enc.encode(bit, hp::clamp_int(p << 4, 1, 65535));
            pred.update(bit);
        }
        if ((k & 0xfffff) == 0 && k) {
            std::fprintf(stderr, "\r%llu MB", (unsigned long long)(k >> 20));
            std::fflush(stderr);
        }
    }
    enc.flush();
}

int compress(const char* inp, const char* outp, hp::Config cfg, bool use_dict,
             bool profile) {
    std::vector<std::uint8_t> raw;
    if (!read_all(inp, raw)) return 1;

    std::vector<std::string> dict;
    std::vector<std::uint8_t> dser, body;
    if (use_dict) {
        dict = hp::dict_build(raw);
        hp::dict_serialise(dict, dser);
        hp::dict_encode(raw, dict, body);
        // Only keep the dictionary if the transform actually shrank the
        // stream by more than the dictionary costs. Honest accounting.
        if (body.size() + dser.size() >= raw.size()) {
            use_dict = false;
            dict.clear();
            dser.clear();
            body = raw;
        }
    } else {
        body = raw;
    }

    std::FILE* out = std::fopen(outp, "wb");
    if (!out) { std::perror(outp); return 1; }

    std::fwrite(kMagic, 1, 4, out);
    std::fputc(kVersion, out);
    std::fputc(cfg.gria ? 1 : 0, out);
    std::fputc(cfg.table_bits, out);
    std::fputc(cfg.mixer_lr, out);
    std::fputc(use_dict ? 1 : 0, out);
    put64(out, raw.size());
    put64(out, body.size());
    put32(out, static_cast<std::uint32_t>(dser.size()));
    if (!dser.empty()) std::fwrite(dser.data(), 1, dser.size(), out);
    const long hdr = std::ftell(out);

    hp::Profiler prof;
    encode_buffer(body, cfg, out, profile ? &prof : nullptr);

    const long csize = std::ftell(out);
    std::fclose(out);

    if (!raw.empty()) {
        const std::uint64_t bits = static_cast<std::uint64_t>(csize) * 8;
        std::fprintf(stderr,
            "\rin %llu B  out %ld B  (hdr+dict %ld)  bpc %llu.%03llu\n",
            (unsigned long long)raw.size(), csize, hdr,
            (unsigned long long)(bits / raw.size()),
            (unsigned long long)((bits * 1000 / raw.size()) % 1000));
        if (use_dict) {
            std::fprintf(stderr, "  dict: %zu words, %zu B, body %zu B (%lld%% of raw)\n",
                dict.size(), dser.size(), body.size(),
                (long long)(body.size() * 100 / raw.size()));
        }
    }
    if (profile) prof.report(stderr, body.size());
    return 0;
}

int decompress(const char* inp, const char* outp) {
    std::FILE* in = std::fopen(inp, "rb");
    if (!in) { std::perror(inp); return 1; }

    char magic[4];
    if (std::fread(magic, 1, 4, in) != 4 || std::memcmp(magic, kMagic, 4) != 0) {
        std::fprintf(stderr, "not a CYHP archive\n");
        std::fclose(in); return 1;
    }
    if (std::fgetc(in) != kVersion) {
        std::fprintf(stderr, "version mismatch\n"); std::fclose(in); return 1;
    }

    hp::Config cfg;
    cfg.gria = std::fgetc(in) != 0;
    cfg.table_bits = std::fgetc(in);
    cfg.mixer_lr = std::fgetc(in);
    cfg.normalize();
    const bool use_dict = std::fgetc(in) != 0;
    const std::uint64_t nraw = get64(in);
    const std::uint64_t nbody = get64(in);
    const std::uint32_t ndict = get32(in);

    std::vector<std::string> dict;
    if (ndict) {
        std::vector<std::uint8_t> dser(ndict);
        if (std::fread(dser.data(), 1, ndict, in) != ndict) {
            std::fprintf(stderr, "truncated dictionary\n"); std::fclose(in); return 1;
        }
        hp::dict_deserialise(dser, dict);
    }

    hp::Predictor pred(cfg);
    hp::Decoder dec(in);
    std::vector<std::uint8_t> body;
    body.reserve(static_cast<std::size_t>(nbody));

    for (std::uint64_t i = 0; i < nbody; ++i) {
        int byte = 0;
        for (int b = 0; b < 8; ++b) {
            const int p = pred.predict();
            const int bit = dec.decode(hp::clamp_int(p << 4, 1, 65535));
            pred.update(bit);
            byte = (byte << 1) | bit;
        }
        body.push_back(static_cast<std::uint8_t>(byte));
        if ((i & 0xfffff) == 0 && i) {
            std::fprintf(stderr, "\r%llu MB", (unsigned long long)(i >> 20));
            std::fflush(stderr);
        }
    }
    std::fclose(in);

    std::vector<std::uint8_t> raw;
    if (use_dict) hp::dict_decode(body, dict, raw);
    else raw = body;

    if (raw.size() != nraw) {
        std::fprintf(stderr, "\nsize mismatch: got %zu want %llu\n",
                     raw.size(), (unsigned long long)nraw);
        return 1;
    }
    std::FILE* out = std::fopen(outp, "wb");
    if (!out) { std::perror(outp); return 1; }
    std::fwrite(raw.data(), 1, raw.size(), out);
    std::fclose(out);
    std::fprintf(stderr, "\rdecompressed %llu B\n", (unsigned long long)nraw);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) return usage();
    const char* mode = argv[1];
    hp::Config cfg;
    // Dictionary defaults OFF: measured -1.28% on the 3 MB proxy because
    // raw dictionary storage (10,121 B) swamped the transform's 1,498 B
    // gain. Expected to flip positive at enwik8/enwik9 scale where it
    // amortises -- re-test there before trusting it. See UPGRADES.md 4.1.
    bool use_dict = false, profile = false;
    int i = 2;
    for (; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--no-gria")) cfg.gria = false;
        else if (!std::strcmp(argv[i], "--no-dict")) use_dict = false;
        else if (!std::strcmp(argv[i], "--dict")) use_dict = true;
        else if (!std::strcmp(argv[i], "--profile")) profile = true;
        else if (!std::strcmp(argv[i], "--mem") && i + 1 < argc) {
            cfg.table_bits = std::atoi(argv[++i]);
            cfg.normalize();
        }
        else if (!std::strcmp(argv[i], "--lr") && i + 1 < argc) cfg.mixer_lr = std::atoi(argv[++i]);
        else break;
    }
    if (argc - i != 2) return usage();
    cfg.normalize();
    if (!std::strcmp(mode, "c")) return compress(argv[i], argv[i + 1], cfg, use_dict, profile);
    if (!std::strcmp(mode, "d")) return decompress(argv[i], argv[i + 1]);
    return usage();
}
