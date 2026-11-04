# HP40 / HP40-Lite general-purpose compression study

**Paper:** [`HP40_paper.md`](HP40_paper.md) (Part I, assembled from `sections/` by `scripts/assemble.py`) and
[`HPD_part2.md`](HPD_part2.md) (Part II: hpd, the improved compressor in [`compress/hpd/`](../../compress/hpd/);
source package [`dist/hpd-1.1.0-src.zip`](../../dist/hpd-1.1.0-src.zip)).

HP40 is champion v40 of the `hp` context-mixing lab
([odin-loki/CompressionAlgorithm](https://github.com/odin-loki/CompressionAlgorithm)).
HP40-Lite is HP40 with 43 of its 75 experts, chosen by ablation.

| | HP40 | HP40-Lite | xz -9e |
|---|---:|---:|---:|
| GP set (27 MB, 42 files), bits/byte | 1.851 | 1.874 | 2.316 |
| vs xz -9e | −20.1% | −19.1% | 0 |
| speed, single job, `--mem 22` | 9.0 KiB/s | 18.8 KiB/s | 1,120 KiB/s |
| peak RSS, `--mem 22` | 10.6 GiB | 6.2 GiB | 76–138 MiB |

## Layout

| path | what |
|---|---|
| `HP40_paper.md` | the paper (generated; edit `sections/` and re-run `scripts/assemble.py`) |
| `sections/` | paper source, one file per section |
| `figures/` | all figures (`scripts/make_figures.py`) |
| `data/` | every measurement: benchmark rows (`*.jsonl`), ablation, mining summaries, profiles, generated tables |
| `scripts/build.sh` | rebuilds HP40, HP40-Lite, Lite-L5 and the mining driver from upstream `0b1ced2` + patch |
| `scripts/bench.py` | compress / decompress / SHA-256 verify, with wall, user, sys and peak RSS per run |
| `scripts/mine.cpp`, `analyze_mine.py`, `expert_names.py` | per-bit expert mining and its analysis |
| `scripts/analyze_abl.py`, `memacct.py`, `perf_components.py`, `touch.cpp` | ablation summary, analytic memory model, perf attribution, first-touch benchmark |
| `scripts/gen_tables.py`, `make_figures.py`, `figstyle.py` | tables and figures from `data/` |
| `patches/hp40_lite_switches.patch` | five default-on switches that make every expert family removable |

## Rebuild

```bash
paper/hp40/scripts/build.sh /tmp/hp40build      # clones upstream, applies patch, builds
/tmp/hp40build/hp_v40 c --mem 22 enwik8.8mb out.hp # must be 1,721,462 bytes
HP_BIN=/tmp/hp40build python3 paper/hp40/scripts/bench.py hp40_m18 rows.jsonl FILE...
python3 paper/hp40/scripts/gen_tables.py && python3 paper/hp40/scripts/make_figures.py && python3 paper/hp40/scripts/assemble.py
```

`scripts/bench.py` runs binaries named `hp_v40`, `hp_v40lite` (and `hp:<name>:<mem>` for any other) from `$HP_BIN` (default `scripts/bin/`); NODEC=1 skips decompression.
