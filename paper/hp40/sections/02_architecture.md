## 2. The systems under test

### 2.1 Lineage and the v40 recipe

`hp` is the integer-exact context-mixing (CM) compressor in
[odin-loki/CompressionAlgorithm](https://github.com/odin-loki/CompressionAlgorithm)
(vendored into Cypha as `native/third_party/hp`, reduced to the later *gate24*
recipe). The upstream tree is an ablation lab: every model and mixer change is
a compile-time switch in `hp/include/hp/features.hpp`, and the lab log
(`RECORD.md`) accepts or rejects one switch at a time on the first 8 MiB of
enwik8 at `--mem 22`. Accepted switches stack into numbered champions
(v5 … v93).

**HP40** is champion **v40** of that log:

> `v40 = v39 + o3/o4+5 — 1,721,462 / 1.641 bpc, RT PASS` (`RECORD.md` line 827)

The repository keeps only the *latest* champion's flag file
(`hp/tools/v78_flags.ps1`, which now tracks v93), and the current upstream
defaults have drifted (most later flags are now default-on and `HP_SLOT_MAX` is
hard-capped at 22). v40 therefore cannot be rebuilt from upstream HEAD. We
rebuilt it from the oldest commit that still has every v40 switch individually
controllable (`0b1ced2`, 2026-09-12, the v47 board) with the default flag set
plus the 39 switches accepted between v5 and v40 and the `SLOT_MAX=32` cap,
walked in order from the log
(`data/v40_flags.txt`):

| step | switch(es) accepted | 8 MiB bytes (log) |
|---|---|---:|
| v5 | `MIXER_SKIP=24`, `SPARSE_UTF8` | 1,798,335 |
| v6–v7 | `SENWORD`, `GATE_ARGMAX` | 1,795,356 |
| v8–v10 | `SLOT_GROW` (replaced v8's `SLOT_SIZES`), `GATE_WORDPOS`, `STATE_TABLE2`, `SLOT_GROW_EXTRA` | 1,781,301 |
| v11–v13 | `SENT_STREAM`, `MATCH_18`, `QUOTE_STACK` | 1,775,314 |
| v14–v25 | `SLOT_WORD2…9` (word/wbi tables +8 bits), `MATCH_01/02`, `WMATCH_4`, `SLOT_S3`, `SLOT_WSTR2`, `SLOT_MAX` 29→31 | 1,747,228 |
| v26–v28 | `SENT_MEM`, `SLOT_S4`, `MATCH_05` | 1,745,709 |
| v29–v40 | `SLOT_O34…O34E`, `SLOT_O6…O6F`, `MATCH_GROW`, `SLOT_MAX=32` | **1,721,462** |

Of the 39 switches, 25 only **grow hash tables** (they add table bits to the
order-3/4/6, word, word-bigram and sentence models). That fact dominates
HP40's memory profile (§5.3).

**Reproduction.** Built with GCC 13.3, `-O3 -march=native -std=c++20`, the
rebuilt HP40 compresses `enwik8.8mb` (SHA-256 `09f6dd72…ee8e`) at `--mem 22`
to **1,721,462 bytes**, byte-identical to the log entry. This is the anchor
for every other number in the paper.

### 2.2 HP40-Lite

HP40-Lite is HP40 with **fewer experts**: the same code, flags and archive
format family, with whole expert families switched off. Five of the families
had no switch upstream (the sparse `sp13/sp24`, column, tag and word-bigram
context models, and the Hebbian expert), so we added five default-on switches
(`patches/hp40_lite_switches.patch`). With every switch at its default the
patched tree reproduces HP40 byte-for-byte: identical archives on three test
files, and it decodes HP40's 8 MiB anchor archive exactly (§4.3). Which families Lite drops is decided by data in §7, not by hand.

### 2.3 Per-bit pipeline

Every byte is coded MSB-first as eight binary decisions; encoder and decoder
run the same predictor, so the compressor is symmetric in time and memory.

```
 byte history ──► EXPERTS (75 stretched logits + bias)
                  │ 18 hashed context models × 2 outputs  (bit-history StateMap + Pitman–Yor direct)
                  │  9 byte-match models (orders 1,2,3,4,5,6,8,10,16)
                  │  1 sparse-UTF-8 match, 4 word-keyed match models
                  │  1 Hebbian word-association expert
                  │ 12 discovery-pool slots × 2 outputs (online MDL-selected context masks)
                  ▼
                MIXER: 10 layer-1 logistic mixers, each gated on a different
                  context (partial byte c0, GRIA entropy regime, previous byte,
                  match length, entropy bucket, Hebbian strength, wiki state,
                  pattern class, loudest-expert identity, in-word position);
                  layer 2 mixes their 10 outputs, gated on c0
                  ▼
                APM ×3 (c0; previous byte×c0; GRIA bucket×c0) → final p (12-bit)
                  ▼
                32-bit binary arithmetic coder
                  ▼
                update(bit): every table, mixer weight and APM learns online
```

There is no floating point anywhere in the codec path, so archives are
bit-reproducible across machines (§4.3).

### 2.4 Expert inventory

| family | models | mixer inputs | context | table bits @ mem 22 |
|---|---|---:|---|---:|
| order-n CMs | o1, o2, o3, o4, o6 | 10 | last n bytes | 22, 22, 27, 27, 28 |
| word CM | word | 2 | hash of current word | **31** |
| sparse CMs | sp13, sp24 | 4 | bytes 1,3 / 2,4 back | 22 |
| column CM | col | 2 | byte above in previous line | 22 |
| tag CM | tag | 2 | markup nesting / wiki context key | 25 |
| word-bigram CM | wbi | 2 | previous word × current word | **31** |
| word-stream CM | wstr | 2 | first-char / previous-word streams | 25 |
| bracket CM | brk | 2 | open-bracket/quote stack | 22 |
| link CM | link | 2 | wiki link word | 25 |
| numeric CM | num | 2 | numeric field state | 22 |
| sentence-word CM | sen | 2 | body-prose word hash | 25 |
| sentence-stream CM | sentst | 2 | sentence-boundary word | 26 |
| sentence-memory CM | sentmem | 2 | hash of a similar earlier sentence | 22 |
| byte match | m1 … m16 | 9 | longest match at 9 orders | buffer 26, index 23 |
| sparse-UTF-8 match | sutf8 | 1 | gap-1 match for escaped UTF-8 | 26 / 23 |
| word match | wm1 … wm4 | 4 | match keyed on 1–4 previous words | 26 / 23 |
| Hebbian | hebb | 1 | learned word→word association | 22 |
| discovery pool | disc0 … disc11 | 24 | online-selected 16-byte masks | 22 |
| bias | — | 1 | partial byte | — |
| **total** | **45 models** | **76** | | |

Most of the families above were built for Wikipedia XML (enwik8/9 is the
Hutter Prize corpus): `tag`, `link`, `brk`, `sen`, `sentst`, `sentmem`, the
wiki-state mixer gate, and the word families. A general-purpose compressor has
to cope with executables, databases, images, PDFs and tarballs too; how these
text-specialist experts behave on such data is one of the questions the paper
answers.
