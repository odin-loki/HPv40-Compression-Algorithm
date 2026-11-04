## 3. Method

### 3.1 Corpora

| set | files | bytes | role |
|---|---:|---:|---|
| **Canterbury** (classic) | 11 | 2,810,784 | small mixed files: text, C, Lisp, HTML, man page, Excel, fax image, SPARC binary |
| **Calgary** (large) | 18 | 3,251,493 | text, papers, source, object code, geophysics data, image, news |
| **Silesia-1M**: first 1 MiB of each Silesia file | 12 | 12,582,912 | modern mix: tar of source, x86 DLL, MRI, databases, PDF, XML, star catalogue, X-ray |
| **enwik8.8mb** | 1 | 8,388,608 | the Hutter-Prize text HP40 was tuned on |
| *General-purpose (GP) set* = all of the above | **42** | **27,033,797** | headline evaluation |
| *Tuning set*: 256 KiB from offset 4 MiB of each Silesia file + 512 KiB of enwik8 at offset 60 MB | 13 | 3,670,016 | Lite expert search only (disjoint from the GP set) |
| Silesia, full (baselines only) | 12 | 211,938,580 | context for the 1 MiB truncation |

Silesia is truncated to 1 MiB per file because HP40 runs at roughly 10 KB/s
(§4.2); the full corpus would take about seven hours per direction per setting.
The tuning set is cut from different offsets of the same files and from a
different region of enwik8, so the expert choice for Lite is not fitted to
the evaluation bytes.

### 3.2 Baselines

gzip 1.12 `-9`; bzip2 1.0.8 `-9`; xz 5.4.5 `-9e` (single thread);
zstd 1.5.5 `-19 --long=27` and `--ultra -22 --long=27` (single thread);
brotli 1.1.0 `-q 11 -w 24`; lz4 1.9.4 `-12`. All run as stdin→stdout
pipes. These are the general-purpose tools a user would otherwise choose.
No other CM compressor (paq8px, cmix) was run, so HP40's standing among CM
compressors is outside the scope of this study.

### 3.3 Settings

HP40 is evaluated at its tuned setting `--mem 22` and swept over
`--mem 16, 18, 20` to map the memory/ratio trade-off. The Lite search runs
at `--mem 18` (700 MB per job, so four jobs fit in RAM; at `--mem 20` four
parallel jobs spent a large share of their time zeroing pages); the chosen Lite is
then evaluated at every setting HP40 is.

### 3.4 Measurements

* **Size**: archive bytes including HP's 29-byte header; bits per byte
  (bpc) = 8 × archive / input.
* **Time**: wall, user and system seconds of the compressor process, taken
  from `wait4()` rusage.
* **Memory**: peak RSS (`ru_maxrss`) of the same process.
* **Correctness**: every decompressed output is compared to the input by
  SHA-256.
* **CPU profile**: `perf record -e cpu-clock` (the VM exposes no hardware
  counters) on the optimised binary, plus a `-fno-inline` build so time can
  be charged to components (context models, match models, mixer, APM, coder).
  The no-inline build distorts absolute time; only its *shares* are used.
* **Expert mining**: a diagnostic driver (`scripts/mine.cpp`) runs the real
  predictor over a file and records, for every coded bit, the cost
  −log2 p of each of the 75 experts' own prediction, the final cost, and
  which expert was cheapest. A 1-in-29 sample of the raw stretched
  predictions feeds the correlation and rank analysis (`analyze_mine.py`).
  The driver links the codec read-only, so it measures exactly what the
  compressor computes.

### 3.5 Platform

One VM: Intel Xeon @ 2.80 GHz, 4 vCPUs (no SMT), 128 KiB L1d / 4 MiB L2 per
core, 33 MiB shared L3, 15 GiB RAM, no swap, Linux 6.18, transparent huge
pages `madvise` (HP does not request them). GCC 13.3.

To finish in reasonable time, the GP-set runs of the HP family
shared the machine with two or three other single-threaded jobs (each job
on its own core, but sharing memory bandwidth and the L3). Their sizes are
exact, but their times (and the baselines', also run alongside other jobs)
are upper bounds. Table 3 repeats the key timings with
one job alone on the machine.
