## 1. Introduction

`hp` is an integer-exact context-mixing compressor written for the Hutter
Prize: every model, gate and size in it was accepted or rejected by its
effect on Wikipedia XML (enwik8). Its champion **v40** (HP40) reached
1,721,462 bytes on the first 8 MiB of enwik8, 25% smaller than xz -9e. A
later descendant (v78, "gate24") is vendored into Cypha as CyphaLM's byte
predictor (`native/third_party/hp`).

This paper asks a different question: **is HP40 a good general-purpose
compressor?** General-purpose means three things a Hutter-Prize score does
not measure:

1. **Ratio on data it was not tuned on**: executables, databases, images,
   spreadsheets, PDFs, tarballs and small files, not just Wikipedia.
2. **Cost**: time, memory and start-up for real file sizes, and how those
   scale with the `--mem` table-size knob.
3. **Efficiency of the model set**: how many of its 75 experts earn their
   keep on general data, and what a lighter variant, **HP40-Lite** (fewer
   experts), gives up.

We answer by reproducing HP40 byte-exactly from the upstream lab, running
it and Lite against seven standard compressors on 42 files (27 MB) from four
corpora at four table sizes, profiling it at the function, instruction and
memory level, and **mining** its experts: we log every expert's prediction
on every coded bit of 18 data types to see which experts win, how
redundant they are, and where the mixing gain comes from. The expert set for
Lite is chosen from leave-one-out and add-back ablations on a separate
tuning set, not by hand.

### Contributions

* A byte-exact rebuild of HP40 from the lab log (39 compile switches and a
  table-size cap over the v47-era tree), with the recipe and a five-switch patch that makes every
  expert family removable (`data/v40_flags.txt`,
  `patches/hp40_lite_switches.patch`).
* The first general-purpose evaluation of HP40: 16 configurations ×
  42 files, every archive size, time and peak RSS recorded
  (`data/*.jsonl`).
* A CPU and memory profile that locates the bottleneck (one random DRAM load per
  context model per bit) and the memory (two 4 GiB word tables), with an
  analytic memory model that matches measured RSS within 4%.
* An expert-level data-mining study over 18 data types: standalone costs,
  win shares, correlation spectra, mixing gains, bit-position costs and
  learning curves.
* **HP40-Lite**, 43 of 75 experts, chosen by measured ablation: 1.2%
  larger than HP40, 44–52% less time, about 40% less memory.
* Concrete engineering recommendations, three of which the upstream lab
  independently adopted after v40.
