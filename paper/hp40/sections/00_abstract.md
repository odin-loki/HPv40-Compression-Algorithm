# HP40 and HP40-Lite as General-Purpose Compressors: A Profiling and Data-Mining Study of an Integer-Exact Context Mixer

**Author:** Odin Loch (with Claude Code)
**Date:** 2026-11-03
**Status:** Research report. All numbers are measured in this study unless cited to the upstream lab log. Raw data, scripts and figures live next to this file (`paper/hp40/`).

---

## Abstract

HP40 is champion v40 of the `hp` Hutter-Prize lab
([odin-loki/CompressionAlgorithm](https://github.com/odin-loki/CompressionAlgorithm)):
an integer-exact, bitwise context-mixing (CM) compressor whose 75 experts and
39 accepted compile switches were all tuned on Wikipedia XML. We rebuild it
byte-exactly (1,721,462 bytes on the first 8 MiB of enwik8, matching the lab
log) and ask whether it is a good **general-purpose** compressor.

On a 27 MB, 42-file mix of the Calgary, Canterbury, Silesia (first MiB of
each file) and enwik8 corpora, HP40 at its tuned setting writes
**6,256,158 bytes (1.851 bits/byte): 20.1% smaller than xz -9e and brotli-11**,
and HP40 or Lite writes the smallest archive on 39 of 41 distinct files. The exceptions are a
fax bitmap, where xz wins by 5.4%, and one 13 KB English paper, where brotli's
dictionary wins by 21 bytes. Its cost is the problem. At `--mem 22` it codes at
**8 KiB/s in both directions** and commits **10.6 GiB** of RAM to every file,
including a 3.7 KB one. Profiling shows it is memory-latency bound: 95% of
context-model time is one random table load per model per bit. Two word tables
with 2³¹ slots each hold 73% of its memory, and on this VM the table zeroing
alone costs 17–36 s per file.

Mining every expert's prediction on every bit of 18 data types shows that
mixing beats the best single expert by 6–72% (median 27%). The 75 expert
logits have an effective rank of only 1.6–7.2. Plain order-1…6 models and an
online context-discovery pool win 76% of all bits. Seven Wikipedia-specific
families win under 1% each.

Leave-one-out and add-back ablations on a separate tuning set yield
**HP40-Lite**, which keeps **43 of the 75 experts**. It drops ten near-redundant
families and shrinks the discovery pool from 12 to 4 slots. Lite is **1.21%
larger** than HP40 (6,332,030 bytes, still 19.1% below xz -9e), and takes
**44–52% less time with about 40% less memory**. It dominates HP40 at every table
size below the largest: Lite at `--mem 22` is smaller and faster than HP40 at
`--mem 18`.

Shrinking the tables is the bigger lever. At `--mem 16` (195 MiB) HP40 still
beats xz -9e by 17.3%. We give six engineering changes, all aimed at layout,
allocation and data-type gating rather than new models. The upstream lab
independently adopted three of them after v40.

**Headline numbers (GP set, 27,033,797 bytes):**

| system | bytes | bits/byte | vs xz -9e | compress speed* | peak RSS |
|---|---:|---:|---:|---:|---:|
| HP40, `--mem 22` | 6,256,158 | 1.851 | −20.1% | 8.3 KiB/s | 10.6 GiB |
| HP40-Lite, `--mem 22` | 6,332,030 | 1.874 | −19.1% | 18.1 KiB/s | 6.2 GiB |
| HP40, `--mem 18` | 6,348,580 | 1.879 | −18.9% | 14.7 KiB/s | 704 MiB |
| HP40-Lite, `--mem 18` | 6,440,845 | 1.906 | −17.7% | 25.1 KiB/s | 418 MiB |
| xz -9e | 7,827,736 | 2.316 | 0 | 1,127 KiB/s | 138 MiB |
| brotli -q11 -w24 | 7,835,477 | 2.319 | +0.1% | 344 KiB/s | 111 MiB |

\*Whole-set wall time including start-up; HP runs shared the 4-core VM
(§4.2). Single-job timings are in Table 3.

## Contents

1. Introduction · 2. The systems under test · 3. Method · 4. Results ·
5. Profiling · 6. Data-mining the experts · 7. Designing HP40-Lite from the
data · 8. Discussion · 9. Conclusion · Appendix A: reproduction · Appendix B: per-file table

---
