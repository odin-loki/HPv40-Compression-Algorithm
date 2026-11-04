## 8. Discussion

### 8.1 Is HP40 a good general-purpose compressor?

**On ratio, yes, decisively.** Although every switch in its recipe was
accepted on Wikipedia XML, HP40 beats xz -9e by 20% on a 27 MB mix of text,
source, executables, databases, images, PDFs and tarballs. It beats xz on
every distinct file except one 1-bit fax image. Context mixing generalises
because the mixer re-weights 75 experts per bit: the text specialists are
simply ignored on binary data (§6), and the generic order-1…6, sparse,
column, match and discovery experts carry it.

**On cost, no, not at its tuned setting.** At `--mem 22` HP40:

* runs at **8 KiB/s** in both directions, about 120–140× slower than xz -9e to
  compress and about 2,800× slower to decompress (CM is
  symmetric; LZ decoders are not);
* commits **10.6 GiB** of RAM for any input, including a 3.7 KB file;
* spends **17–36 s of fixed start-up** per file on this VM zeroing that
  memory (a few seconds on a host with free RAM, §5.3), which dominates every
  file under ~100 KB (Figure 4);
* is single-threaded and keeps no block structure, so it cannot use more cores
  and cannot seek.

The tuned setting is the wrong setting for general use. `--mem 18` keeps
**94%** of the ratio advantage (−18.9% vs xz instead of −20.1%; archives
1.5% larger) in 0.7 GiB with sub-second start-up at 1.8× the speed (Table 2). HP40-Lite at
`--mem 18` gives up another 1.5% for 44% less time and 41% less memory.
These are the configurations to ship.

### 8.2 What the profile says to fix

The measurements point at six concrete changes. None of them requires a new
model.

1. **Size tables to the input.** Ratio is flat in table size once the tables
   exceed the input's distinct contexts (Figure 2), so `--mem` should be set
   from the file size (e.g. `table_bits ≈ log2(input bytes) − 2`, clamped
   16–22). That alone removes the fixed start-up from small files.
2. **Allocate lazily.** The tables are `std::vector`s value-initialised to
   zero, so every page is written before the first byte is coded.
   Allocating with `calloc`/anonymous `mmap` would let the kernel supply zero
   pages on first touch: start-up would go to near zero and RSS would track
   what the input actually touches.
3. **Cap the two word tables.** `SLOT_WORD2…9` give the word and word-bigram
   models 2³¹ slots each at `--mem 22`: 8 GiB, 73% of the footprint, for
   two experts that win 3.4% of bits (§6.2). The +8 bits were accepted on
   8 MiB of Wikipedia, where the vocabulary is large; on general data the two
   models win 3.4% of bits, and removing the word-bigram model alone costs
   0.13% (§7).
4. **Share the match history.** Fourteen match models each keep a private
   64 MiB copy of the same input history (0.9 GiB).
5. **Detect incompressible runs.** On compressed members (`mozilla`) HP40
   codes at 7.99 bits/byte at full per-bit cost. A stored-block escape would
   save the time with no loss of ratio.
6. **Gate experts by data type.** The word-keyed experts are 3–8× worse than
   the mix on every binary file, yet are evaluated on every bit. Switching
   expert families on a cheap text/binary detector would give Lite's speed on
   binary data and HP40's ratio on text.

Upstream has since made changes 2 and (in effect) 3: the current lab code
allocates demand-zero hash tables via `mmap`/`VirtualAlloc` and hard-caps
`HP_SLOT_MAX` at 22, which stops the word tables growing past 2²² slots
(`RECORD.md`, "Throughput / RAM (kVersion 5)"). Change 4 landed in commit
`058b25d`. HP40 predates all three.

On ratio, the evidence says where the remaining gains are. HP40 is weakest
relative to xz on the fax bitmap (no 2-D image model), the star catalogue
`sao` (fixed records of binary floats: highest effective rank, lowest mixing
gain) and `mozilla` (compressed members). The PAQ family handles these with
image, record-length and executable (E8/E9) models; HP40 has none.

### 8.3 HP40 vs HP40-Lite

Lite removes 32 of 75 experts. Five families were dead weight on general
data (link, numeric, Hebbian, order-5 and order-8 matches: under 0.01% each
when removed). The word-oriented cluster is
redundant as a group but not individually: drop one and the others cover;
drop all and the loss is 60% more than the parts. The discovery pool is the
largest single CPU cost and, on record-structured binary data, a large ratio
contributor. The column model is a specialist no other expert replaces.

For a user the choice is simple: Lite when time or memory matters (1.2–1.5%
larger, 44–52% less time, about 40% less memory at every setting), HP40
when the last percent matters. The one data type where Lite falls well
behind HP40 is the spreadsheet `kennedy.xls` (§6.5); it still beats every
baseline there.

### 8.4 Threats to validity

* **One machine.** A 4-vCPU VM without hardware counters; some runs shared the
  box, and timings of runs made under load are marked. Ratios are exact and
  machine-independent (the codec has no floating point).
* **Truncated Silesia.** 1 MiB per Silesia file favours fast learners;
  CM usually gains more on long inputs, so these numbers likely understate
  HP40's ratio advantage on the full corpus.
* **Tuning and evaluation share source files.** Lite was chosen on
  different 256 KiB regions of the same Silesia files plus enwik8. Lite's
  evaluation-set loss (1.21%, Table 1) is slightly below its tuning-set
  loss (1.48%), which suggests no overfit, but other data could rank the families differently.
* **Search at `--mem 18`.** The ablation and the mining ran at `--mem 18`;
  Lite's loss at `--mem 22` is reported separately and agrees.
* **Rebuild.** HP40 was rebuilt from the v47-era tree with the v40 switches.
  It is byte-identical on the anchor (1,721,462 B), which is strong but not
  complete evidence that every code path matches the original v40 binary
  (paths that enwik8 never exercises could in principle differ).
* **Baselines.** No other CM compressor (paq8px, cmix) was run; HP40's place
  among CM compressors is not measured here.
