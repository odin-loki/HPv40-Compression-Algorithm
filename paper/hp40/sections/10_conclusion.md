## 9. Conclusion

HP40 is a very good compressor that was never meant to be a general-purpose one.
Tuned only on Wikipedia, it still compresses an unrelated 27 MB mix 20%
smaller than xz -9e and brotli-11, and loses to them on only two of 41
distinct files. Its model is general: the mixer quietly ignores the text
experts on binary data, and the generic order-n, sparse, column, match and
self-discovering context experts do the work. Its *engineering* is not general:
at its tuned setting it commits 10.6 GiB and tens of seconds of start-up to
every file, runs at 8 KiB/s in both directions, and spends a third of its time
waiting on one random DRAM read per context model per bit.

Three findings carry over to any deployment:

1. **Use `--mem 16–18`, not 22.** Ratio drops 1.5–3.5%; memory drops 16–56×
   and start-up disappears.
2. **Use HP40-Lite.** With 43 of 75 experts it is 1.2% larger than HP40 and
   takes 44–52% less time with about 40% less memory. Lite at `--mem 22` is smaller
   *and* faster than HP40 at `--mem 18`, and Lite at 20 beats HP40 at 16.
3. **Fix the table layout before adding models.** Cache-line buckets,
   demand-zero allocation, input-sized tables, shared match history and a
   stored-block escape address every cost this study measured without
   touching the model.

The expert mining also gives the lab a ranking it lacked: the order-1…6
models and the discovery pool win 76% of all bits; the discovery pool is the
component that adapts HP40 to data nobody wrote a model for (it alone is worth
22% on a spreadsheet); and seven text families win under 1% of bits each on
general data.

**Follow-up.** [Part II](HPD_part2.md) applies these findings: **hpd**
(`compress/hpd/`) is HP40-Lite with bucketed, prefetched tables,
demand-zero memory, a record model, an x86 transform and parallel blocks.
It is 1.55% smaller than HP40 on the GP set, 5.6× faster and uses 1/18 of
the memory, and 19.4% smaller than xz -9e on the full Silesia corpus.
Its source is packaged as
[`dist/hpd-1.0.0-src.zip`](../../dist/hpd-1.0.0-src.zip) (Part II, §9).

## Appendix A. Reproducing this paper

```bash
# 1. binaries (clones CompressionAlgorithm @ 0b1ced2, applies the switch patch)
paper/hp40/scripts/build.sh /tmp/hp40build
# 2. anchor: must print 1721462
/tmp/hp40build/hp_v40 c --mem 22 enwik8.8mb out.hp && stat -c %s out.hp
# 3. one benchmark row: compress, decompress, verify (binaries from $HP_BIN)
HP_BIN=/tmp/hp40build python3 paper/hp40/scripts/bench.py hp40_m18 rows.jsonl FILE...
# 4. expert mining for one file, then the summary
/tmp/hp40build/mine_v40 FILE out/v40_NAME 18 29 300000 65536
python3 paper/hp40/scripts/analyze_mine.py v40 mine_v40.json out/v40_NAME
# 5. every table and figure in this paper from data/
python3 paper/hp40/scripts/gen_tables.py && python3 paper/hp40/scripts/make_figures.py
```

Corpora: enwik8 (mattmahoney.net / data.deepai.org mirror), Canterbury
(`bench/data/canterbury/cantrbry.tar.gz` in this repo), the large Calgary
corpus (data-compression.info), Silesia (sun.aei.polsl.pl). `enwik8.8mb` is
`head -c 8388608 enwik8` (SHA-256 `09f6dd72…ee8e`); Silesia-1M is
`head -c 1048576` of each Silesia file; the tuning set is
`tail -c +4194305 FILE | head -c 262144` per Silesia file plus
`tail -c +60000001 enwik8 | head -c 524288`.

### Data files

| file | contents |
|---|---|
| `data/v40_flags.txt`, `data/lite_flags.txt`, `data/lite_L*.txt` | compile recipes |
| `data/gp_*.jsonl`, `data/base_gp.jsonl`, `data/base_silfull.jsonl` | every benchmark row: sizes, wall/user/sys, peak RSS, round-trip |
| `data/clean_timing.jsonl` | single-job timings (Table 3) |
| `data/abl_m18.jsonl`, `data/addback_m18.jsonl`, `data/ablation_summary.*`, `data/kennedy_m18.jsonl` | Lite search |
| `data/mine_v40.json`, `data/mine_lite.json`, `data/corr_v40_*.json` | expert mining summaries and correlation matrices |
| `data/*_ni_*.json`, `data/first_touch.txt`, `data/startup*.jsonl` | CPU profile shares, page-zeroing and start-up measurements |
| `data/tables.md`, `data/gp_summary.json` | generated tables |
