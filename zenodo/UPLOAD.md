# Uploading to Zenodo

Everything to upload is in **`zenodo/upload/`** (rebuild it any time with
`python zenodo/make_package.py`):

| file | size | what |
|---|---:|---|
| `HP40_hpd_paper.pdf` | 2.2 MB | the paper: Parts I and II as one, 25 pages |
| `hpd-1.2.0-src.zip` | 366 KB | hpd source package (cited in §18 of the paper) |
| `HP40-hpd-paper-materials-1.2.0.zip` | 5.6 MB | paper sources (Markdown), data, figures, scripts, patch, PDF build |
| `SHA256SUMS.txt` | | SHA-256 of the three files |

```
fe8a60d648b27acdcc04a1562e9c954045da929791adff45dbffef6c1f4f16ab  HP40-hpd-paper-materials-1.2.0.zip
e24e75060df30be5dedc4fd9855654362099fc77afa658e65d8af97ad1d257f7  HP40_hpd_paper.pdf
7e11febcd342c077769d92dfb95be9c6981378925b67c03cc68dc4c654279d01  hpd-1.2.0-src.zip
```

## Steps

1. Sign in at <https://zenodo.org> (GitHub, ORCID or e-mail). To rehearse
   first, use <https://sandbox.zenodo.org>; it behaves the same and nothing
   there is permanent.
2. Click **New upload** (top right).
3. **Files:** drag in the four files from `zenodo/upload/`. Set
   `HP40_hpd_paper.pdf` as the default preview if Zenodo asks.
4. **DOI:** at "Do you already have a DOI?" answer **No**, then click
   **Get a DOI now!**. The DOI is reserved but not active until you publish.
   *Optional:* send me the reserved DOI before you publish and I will print it
   in the paper and rebuild the PDF (the source zip does not change; the PDF and
   materials zip get new checksums, so re-upload those two).
5. **Metadata:** copy the fields below.
6. *(Optional)* **Communities:** add the record to a compression or
   open-source software community.
7. **Preview**, check the files and text, then **Publish**. A published
   record's files cannot be changed; a correction is a **New version** (one
   DOI for all versions plus one per version).

## Metadata (copy and paste)

**Resource type:** Publication → Report
*(alternative: Publication → Preprint; the software is attached either way)*

**Title:**
```
HP40 and hpd: Profiling, Data-Mining and Rebuilding an Integer-Exact Context Mixer for General-Purpose Compression
```

**Publication date:** `2026-11-05`

**Creators:** Loch, Odin — add your ORCID if you have one; affiliation optional.
The paper's byline reads "Odin Loch (with Claude Code)"; keep or change that
before publishing (it is in `paper/hp40/sections/00_abstract.md`,
`paper/hp40/HPD_part2.md` and `paper/hp40/scripts/combine.py`).

**Description:**
```
HP40, champion v40 of the hp Hutter-Prize lab, is an integer-exact bitwise context-mixing compressor with 75 experts tuned only on Wikipedia XML. Part I rebuilds it byte-exactly and evaluates it as a general-purpose compressor on 42 files (27 MB) from the Calgary, Canterbury, Silesia and enwik8 corpora: it writes 1.851 bits/byte, 20.1% smaller than xz -9e and brotli-11, but codes at 8 KiB/s and commits 10.6 GiB per file. Profiling shows it is memory-latency bound (one random DRAM load per context model per bit). Mining every expert's prediction on every bit of 18 data types shows mixing beats the best single expert by 6–72% and the experts have an effective rank of only 1.6–7.2. Ablations give HP40-Lite (43 of 75 experts): 1.2% larger, 44–52% less time, about 40% less memory.

Part II applies these findings. hpd is HP40-Lite rebuilt around the profile (cache-line buckets with checksums, demand-zero tables, a record model, an x86 transform, pass-through of incompressible data, parallel blocks, CRC-32 container). It is 1.55% smaller than HP40, 5.6x faster and uses 1/18 of its memory: 1.823 bits/byte on the GP set (21.3% below xz -9e) and 1.474 bits/byte on the full 212 MB Silesia corpus (19.4% below xz -9e), the smallest archive of every tested compressor on 40 of 41 GP files and all 12 Silesia files. A GPU build writes byte-identical archives; with one warp per shard an RTX 3090 reaches 0.84–0.95x the speed of a 64-thread CPU.

Files: the paper (PDF), the hpd 1.2.0 source package (C++20, Linux/Windows, optional CUDA build; SHA-256 7e11febcd342c077769d92dfb95be9c6981378925b67c03cc68dc4c654279d01), and the paper's materials (Markdown sources, every measurement, figures, analysis scripts and the HP40-Lite patch).
```

**Version:** `1.2.0`

**Language:** English

**Keywords** (one per entry):
```
data compression
lossless compression
context mixing
arithmetic coding
benchmarking
profiling
GPU
HP40
hpd
```

**Licence:** GNU Affero General Public License v3.0 — the repository's
`LICENSE` is the plain AGPL v3 text; pick "only" or "or later" in Zenodo to
match what you intend.

**Related works:**

| relation | identifier | type |
|---|---|---|
| Is supplemented by | `https://github.com/odin-loki/HPv40-Compression-Algorithm` | Software |
| Is derived from | `https://github.com/odin-loki/CompressionAlgorithm` | Software |

**Software** section (if shown): repository
`https://github.com/odin-loki/HPv40-Compression-Algorithm`, programming
language C++, development status Active.

## After publishing

Send me the DOI and I will add it, with a Zenodo badge, to `README.md`,
`compress/hpd/README.md` and the paper, then push.
