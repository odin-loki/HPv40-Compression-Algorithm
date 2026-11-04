## 4. Results

### 4.1 Compression ratio

Table 1 gives archive bytes per corpus; bits per byte are in brackets
(full per-file table: `data/tables.md`, Table C; Figure 1).

**Table 1. Archive bytes on the general-purpose set (27,033,797 bytes, 42 files)**

| compressor | Calgary | Canterbury | Silesia-1M | enwik8.8mb | **total** | vs xz -9e |
|---|---:|---:|---:|---:|---:|---:|
| **HP40** (mem 22) | 697,974 (1.717) | 364,487 (1.037) | 3,472,235 (2.208) | 1,721,462 (1.642) | **6,256,158 (1.851)** | **−20.1%** |
| **HP40-Lite** (mem 22) | 704,110 (1.732) | 372,811 (1.061) | 3,514,543 (2.234) | 1,740,566 (1.660) | **6,332,030 (1.874)** | **−19.1%** |
| HP40 mem 20 | 699,139 (1.720) | 365,527 (1.040) | 3,490,323 (2.219) | 1,731,114 (1.651) | 6,286,103 (1.860) | −19.7% |
| Lite mem 20 | 706,059 (1.737) | 374,569 (1.066) | 3,530,600 (2.245) | 1,751,417 (1.670) | 6,362,645 (1.883) | −18.7% |
| HP40 mem 18 | 703,342 (1.731) | 367,890 (1.047) | 3,519,083 (2.237) | 1,758,265 (1.677) | 6,348,580 (1.879) | −18.9% |
| Lite mem 18 | 709,785 (1.746) | 377,800 (1.075) | 3,570,896 (2.270) | 1,782,364 (1.700) | 6,440,845 (1.906) | −17.7% |
| Lite-L5 mem 18 | 708,536 (1.743) | 371,905 (1.059) | 3,530,806 (2.245) | 1,782,178 (1.700) | 6,393,425 (1.892) | −18.3% |
| HP40 mem 16 | 712,072 (1.752) | 372,843 (1.061) | 3,572,652 (2.271) | 1,816,118 (1.732) | 6,473,685 (1.916) | −17.3% |
| Lite mem 16 | 720,215 (1.772) | 384,046 (1.093) | 3,619,512 (2.301) | 1,846,659 (1.761) | 6,570,432 (1.944) | −16.1% |
| xz -9e | 883,752 (2.174) | 493,080 (1.403) | 4,147,436 (2.637) | 2,303,468 (2.197) | 7,827,736 (2.316) | 0 |
| brotli -q11 -w24 | 856,672 (2.108) | 490,662 (1.397) | 4,199,624 (2.670) | 2,288,519 (2.183) | 7,835,477 (2.319) | +0.1% |
| bzip2 -9 | 866,501 (2.132) | 542,710 (1.545) | 4,102,930 (2.609) | 2,448,077 (2.335) | 7,960,218 (2.356) | +1.7% |
| zstd --ultra -22 --long | 922,123 (2.269) | 521,080 (1.483) | 4,473,396 (2.844) | 2,365,633 (2.256) | 8,282,232 (2.451) | +5.8% |
| zstd -19 --long | 922,148 (2.269) | 521,080 (1.483) | 4,474,482 (2.845) | 2,365,695 (2.256) | 8,283,405 (2.451) | +5.8% |
| gzip -9 | 1,059,335 (2.606) | 730,625 (2.079) | 5,021,623 (3.193) | 3,090,418 (2.947) | 9,902,001 (2.930) | +26.5% |
| lz4 -12 | 1,248,336 (3.071) | 935,398 (2.662) | 5,564,138 (3.538) | 3,545,863 (3.382) | 11,293,735 (3.342) | +44.3% |

* **HP40 is 20.1% smaller than xz -9e** and brotli-11 over the whole set,
  and 16–26% smaller on every corpus. Its lead is largest on its home ground
  (enwik8 −25.3%, Canterbury −26.1%) and smallest on Silesia (−16.3%).
* **Per file** (41 distinct files; `pic` and `ptt5` are the same bytes),
  HP40 or Lite is the smallest archive on **39**. The exceptions:
  - the CCITT fax bitmap `pic` (xz 39,860 vs HP40 42,014, **+5.4%**): HP40 has
    no two-dimensional image context, and LZ's long matches over runs of
    white scan lines win;
  - Calgary `paper4` (13 KB of English), where brotli's built-in English
    dictionary edges out HP40 by 21 bytes (4,283 vs 4,304).
* **The gains are smallest where there is least structure for a context
  model to find**: `mozilla` −3.1% (a tar whose first MiB is mostly
  already-compressed members), `sao` −6.1% (binary star-catalogue records),
  `sum` −10.9%, `geo` −11.7% and `obj1` −12.8% (small object code and float
  data).
* **The gains are largest on structured data**: `kennedy.xls` **−63.0%**
  (19,214 vs 51,868 bytes), `nci` −32.2%, `webster` −30.4%, `reymont` (PDF)
  −30.0%, `xml` −29.2%.
* **HP40-Lite** is 1.21% larger than HP40 overall (Calgary +0.9%, Canterbury
  +2.3%, Silesia-1M +1.2%, enwik8 +1.1%). Without `kennedy.xls` the gap is
  1.13%. Lite is *smaller* than HP40 on the two images (`mr`, `pic`).
  The 1.21% on the evaluation set is *below* the 1.48% it cost on the
  disjoint tuning set, so the expert selection did not overfit.
* **Table size matters little.** From `--mem 22` down to 16, HP40 grows
  by only 0.5% (mem 20), 1.5% (mem 18) and 3.5% (mem 16), while its memory
  shrinks 56× (Table 2). At 195 MiB HP40 still beats xz -9e by 17.3%.

![Figure 1. Archive size saving over xz -9e for each of the 42 files (positive = smaller than xz).](figures/perfile_vs_xz.png)

*Figure 1. Archive size saving over xz -9e for each of the 42 files (positive = smaller than xz).*

![Figure 2. The `--mem` sweep: ratio is nearly flat while memory grows 4× per step.](figures/mem_sweep.png)

*Figure 2. The `--mem` sweep: ratio is nearly flat while memory grows 4× per step.*

### 4.2 Speed and memory

**Table 2. Time and memory on the GP set.** HP rows ran with two or three
other HP jobs on the 4-core VM (§3.5), so their wall times are upper
bounds; Table 3 has clean single-job timings.

| compressor | compress | KiB/s | decompress | peak RSS | round trip |
|---|---:|---:|---:|---:|---|
| HP40 (mem 22) | 53.0 min | 8.3 | ≈ compress | 10.6 GiB | 8 MiB anchor byte-exact (§4.3) |
| HP40-Lite (mem 22) | 24.4 min | 18.1 | ≈ compress | 6.2 GiB | — |
| HP40 mem 20 | 32.8 min | 13.4 | ≈ compress | 2.7 GiB | — |
| Lite mem 20 | 19.2 min | 23.0 | ≈ compress | 1,598 MiB | — |
| HP40 mem 18 | 30.0 min | 14.7 | 29.4 min | 704 MiB | all 42 byte-exact |
| Lite mem 18 | 17.5 min | 25.1 | 17.0 min | 418 MiB | all 42 byte-exact |
| HP40 mem 16 | 25.6 min | 17.2 | ≈ compress | 195 MiB | — |
| Lite mem 16 | 13.9 min | 31.7 | ≈ compress | 123 MiB | — |
| xz -9e | 23.4 s | 1,127 | 0.8 s | 138 MiB | all 42 byte-exact |
| brotli -q11 -w24 | 76.8 s | 344 | 0.3 s | 111 MiB | all 42 byte-exact |
| zstd --ultra -22 --long | 59.3 s | 446 | 0.3 s | 660 MiB | all 42 byte-exact |
| bzip2 -9 | 3.0 s | 8,841 | 1.9 s | 18 MiB | all 42 byte-exact |
| gzip -9 | 3.2 s | 8,296 | 0.4 s | 18 MiB | all 42 byte-exact |

"≈ compress": decompression was not timed on the whole set for these rows.
CM decoding repeats the encoder's work bit for bit, and where it was timed
(mem 18 rows, Table 3) it took the same time as compression.

**Table 3. Single-job timings** (one job alone on the VM; enwik8.1mb + Silesia ooffice + Calgary book1 = 2,865,923 bytes; `data/clean_timing*.jsonl`)

| compressor | bpc | compress wall | of which kernel | KiB/s (wall) | KiB/s (user CPU only) | decompress wall | peak RSS | round trip |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| HP40, mem 22 | 2.296 | 310.3 s | 67.4 s | 9.0 | 11.6 | 294.9 s | 10.6 GiB | 3/3 byte-exact |
| HP40-Lite, mem 22 | 2.326 | 148.5 s | 11.4 s | 18.8 | 20.6 | 149.7 s | 6.2 GiB | 3/3 byte-exact |
| HP40, mem 18 | 2.351 | 202.6 s | 1.4 s | 13.8 | 14.0 | 201.2 s | 690 MiB | 3/3 byte-exact |
| HP40-Lite, mem 18 | 2.391 | 114.3 s | 0.8 s | 24.5 | 24.9 | 113.0 s | 404 MiB | 3/3 byte-exact |
| xz -9e | 2.918 | 2.5 s | 1.0 s | 1120.5 | 1905.8 | 0.1 s | 76 MiB | 3/3 byte-exact |
| brotli -q11 -w24 | 2.897 | 8.6 s | 0.4 s | 327.2 | 349.4 | 0.1 s | 31 MiB | 3/3 byte-exact |
| zstd --ultra -22 --long | 3.029 | 8.2 s | 6.4 s | 341.2 | 1613.3 | 0.0 s | 653 MiB | 3/3 byte-exact |
| bzip2 -9 | 2.919 | 0.3 s | 0.0 s | 10182.5 | 10954.9 | 0.2 s | 17 MiB | 3/3 byte-exact |

Run alone, HP40 at `--mem 22` codes **9.0 KiB/s** (11.6 KiB/s counting user
CPU only), and 22% of its wall time is the kernel zeroing tables. Lite takes
**52% less time** at `--mem 22` and **44% less** at `--mem 18`. HP40 is also
slower at mem 22 than at mem 18 *excluding* kernel time (11.6 vs 14.0 KiB/s of
user CPU): bigger tables mean more cache misses (§5.2). Decompression takes
the same time as compression in every case. xz -9e compresses about 124×
faster than HP40 and decompresses about 2,800× faster.

Figure 3 places every compressor on a ratio/speed plane. HP40 and Lite sit
alone in the bottom-left corner, about 0.46 bits per byte below the best
LZ/BWT compressors and two orders of magnitude slower. Within the HP family, **a Lite setting
beats HP40 at mem 18 and at mem 16 on both axes**: Lite at mem 22 is smaller
(1.874 vs 1.879 bpc) and faster than HP40 at mem 18, and Lite at mem 20 is
smaller (1.883 vs 1.916) and faster than HP40 at mem 16. Full HP40 is only
worth running at mem 20–22, when the last 1% matters more than time and memory.

Figure 4 shows time against file size. At `--mem 22`, HP40 spends 17–36 s
on any file however small, so every file under about 100 KB costs about the
same. At `--mem 18` the floor drops below 1 s. The cause is table zeroing
(§5.3).

![Figure 3. Ratio against compression speed. (a) all compressors; (b) the HP family across `--mem`.](figures/pareto.png)

*Figure 3. Ratio against compression speed. (a) all compressors; (b) the HP family across `--mem`.*

![Figure 4. Compression time against input size: a fixed start-up floor at `--mem 22`.](figures/time_vs_size.png)

*Figure 4. Compression time against input size: a fixed start-up floor at `--mem 22`.*

### 4.3 Correctness

* **Anchor:** the rebuilt HP40 writes the 1,721,462-byte archive the upstream log
  records for v40 on `enwik8.8mb` (encode 820–902 s, 10.6 GiB). That archive,
  decoded by the binary that `scripts/build.sh` produces from the *patched*
  tree, returns the original 8,388,608 bytes (SHA-256 `09f6dd72…ee8e`) in
  779 s. This verifies the `--mem 22` round trip, and shows that the Lite
  switches, when left on, do not change HP40 anywhere in the 8 MiB stream.
* **Round trip:** all 84 HP40 and Lite archives of the GP set at `--mem 18`
  decode to SHA-256-identical files, as do all baseline archives.
* **Determinism:** repeated runs and rebuilds (`-O1` and `-O3`) produce
  identical archives; the codec has no floating point.
