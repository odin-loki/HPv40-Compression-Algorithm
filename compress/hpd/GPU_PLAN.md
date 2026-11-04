# hpd on the GPU: sharded context mixing — plan

**Status:** plan only. Nothing here has run on a GPU yet. The build VM has no
GPU, and OdinsPC is not reachable from cloud sessions. All numbers are CPU
measurements or estimates marked as such.
**Code today:** `compress/hpd/` (CPU, multi-threaded, archive format v2).
**Background:** `paper/hp40/HP40_paper.md` (Part I), `paper/hp40/HPD_part2.md` (Part II).

## 1. Why sharding is the only way onto a GPU

Context mixing is serial inside a stream: every bit's prediction depends on
the model state left by the previous bit. A GPU cannot speed up one stream.
It can run **thousands of independent streams (shards) at once**. So:

* split the input into shards (hpd already does this: `-b MiB` / `-B KiB`);
* give every shard its own model state on the GPU;
* run all shards in lock-step, one bit at a time.

The cost is ratio: each shard starts learning from zero. That cost is
measured, not guessed (§2), and it decides how small shards can be.

## 2. What sharding costs (measured on CPU)

64 MiB test bed = first 16 MiB of Silesia webster + mozilla + nci + samba,
`hpd c -t 4 -B <KiB>`, every round trip byte-exact
(`tools/pc_shard_bench.sh` reproduces this on any machine):

| shard size | shards | archive bytes | bits/byte | vs 16 MiB shards |
|---:|---:|---:|---:|---:|
| 16 MiB | 4 | 10,960,224 | 1.307 | 0 |
| 4 MiB | 16 | 11,264,237 | 1.343 | +2.8% |
| 1 MiB | 64 | *running* | | |
| 256 KiB | 256 | *running* | | |

Earlier single-file points: enwik8 8 MiB as 2 shards +2.7%, as 4 shards +6.0%;
lcet10 (427 KB) as 256 KiB shards +7.5%. For reference, hpd with no sharding
is 19–21% smaller than xz -9e, so roughly **10% of shard loss still beats
xz -9e by ~10%**.

**Decision rule for P0:** pick the smallest shard size whose loss is ≤ 5%
on the PC's own data. Expected: 1–4 MiB.

## 3. Per-shard state (what has to live on the GPU)

From the current code (`include/hp/predictor.hpp`, `sizeof(Predictor)` =
32 KB inline, plus heap tables):

| state | size per shard today | GPU plan |
|---|---:|---|
| ~20 bucketed context tables (13 CMs, 3 record, 4 discovery) | ≈ 2–4 × shard bytes each, capped (`-g 2`) | size to shard: total ≈ 8–16 × shard |
| mixer: 11 layer-1 weight-set families × 68 inputs × int32 | ≈ 200 KB | int16 weights → 100 KB |
| APM lex (65,536 × 33 × u16) | 4.3 MB | shrink to 4,096 contexts (270 KB) or share read-only after warm-up; measure |
| APM GRIA + c0 | 0.4 MB | keep |
| 20 StateMaps (882 × u32) | 70 KB | keep (hot: shared memory) |
| match history + indexes | ≈ shard + 4 × 2^bits × 7 | shard bytes + small index |

Budget target: **≤ 16 MB per 1 MiB shard**, so a 24 GB card holds ~1,000
shards = 1 GB of input in flight.

## 4. Kernel design

* **One warp per shard.** The 32 lanes split the per-bit work:
  * lanes 0–19: one bucketed model each (hash, bucket select, StateMap,
    Pitman–Yor), the lookups that are parallel across models;
  * lanes 20–27: match / word-match / record predictions;
  * the mixer: 11 dot products of 52 inputs. Each lane holds 2 inputs and
    the 11 sums come from `__shfl_xor_sync` reductions;
  * lane 0: APMs, arithmetic coder, bit output (the serial tail).
* **Update phase** mirrors predict: lanes update their own model, mixer
  rows are updated lane-parallel.
* **Byte-boundary work** (context hashes, record detector, discovery-pool
  MDL review) runs once per 8 bits, lane-parallel where possible.
* **State layout:** structure-of-arrays in global memory per shard; mixer
  rows for the current gate contexts and the StateMaps in shared memory.
* **Batching:** host streams shards in batches sized to GPU memory,
  overlapping copy-in, kernel and copy-out with CUDA streams.

### Exactness (non-negotiable)

The codec is integer-only. A GPU shard must produce **the same bytes** as
the CPU coding the same shard with the same settings. Then:

* archives from the GPU decode on the CPU and vice versa;
* every phase has a byte-exact acceptance test against the CPU build.

This rules out float shortcuts and any reordering that changes integer
results (e.g. a different summation order is fine for int64 sums, but
saturation/clamp order must match).

## 5. Phases

| phase | deliverable | acceptance |
|---|---|---|
| **P0** measure | run `compress/hpd/tools/pc_shard_bench.sh <file>` in WSL2 on OdinsPC (or the `.ps1` in PowerShell); record GPU model, VRAM, CUDA version, shard-loss table | `pc_results.txt` committed |
| **P1** flat state | refactor `hp::Predictor` into a POD state struct + `__host__ __device__` `predict(state, …)` / `update(state, …)` functions over flat arrays (no `std::vector`, `shared_ptr`, `thread_local`, `mmap` inside the model) | CPU `hpd` archives byte-identical to v2 on the GP set; `make test` passes |
| **P2** naive kernel | CUDA kernel, **one thread per shard**, calling the P1 functions | GPU archive bytes == CPU archive bytes for every shard of the 64 MiB test bed |
| **P3** warp per shard | parallel predict/update across lanes (§4) | byte-exact as P2; Nsight Compute profile committed |
| **P4** memory | int16 mixer weights, smaller APM, shared-memory hot state; occupancy tuning | ratio change ≤ 0.2% on GP set (CPU build gets the same change); throughput ≥ 2× P3 |
| **P5** product | `hpd c --gpu`, `hpd d --gpu`; falls back to CPU when no GPU; same archive format (v3 if P4 changes the model) | round trips CPU↔GPU both ways on GP set + full Silesia |
| **P6** results | Part III write-up: GPU vs CPU hpd vs xz/zstd/brotli, by shard size | numbers from measured runs only |

## 6. Expected throughput (estimate, not measured)

Per bit, per shard: ~20 table lookups, 11 × 52 mixer MACs plus the same again
to update, ~20 StateMap updates, 3 APMs. The mixer touches ≈ 2.3 KB of
weights to read and ≈ 2.3 KB to write per bit (int32), about 37 KB per input
byte, which makes it **memory-bandwidth bound**:

* int32 weights from DRAM: ~1 TB/s ÷ 37 KB/byte ≈ **25–30 MB/s** per GPU;
* int16 weights, hot rows cached: **50–100 MB/s** is plausible.

Compare CPU hpd at ~80 KB/s per core (~0.3 MB/s on 4 cores) and xz -9e at
~1.9 MB/s. If P4 lands near the estimate, a GPU hpd would be roughly 10–50×
faster than xz -9e while staying ~10% smaller. **Decompression runs at the
same speed as compression** (CM is symmetric). It would still be far slower
than xz decompression, which runs at 100+ MB/s.

## 7. Risks

* **Shard loss** larger than measured on the PC's data → bigger shards →
  fewer in flight → less speed. P0 settles it.
* **Memory per shard** too large → occupancy too low. P4's shrink of the
  4.3 MB APM is the first lever.
* **Divergence**: shards finish at different times (stored vs CM blocks,
  last short shard). Mitigation: equal-size shards and stored blocks
  handled on the CPU.
* **Exactness bugs** are the main engineering risk; the byte-exact gates in
  P1–P5 exist for that.
* **Small files** (< a few MiB) gain nothing: one or two shards. hpd should
  keep the CPU path for them.

## 8. Files

| file | role |
|---|---|
| `compress/hpd/src/hpd.cpp` | CLI, container, blocks/shards (`-b`, `-B`), threads |
| `compress/hpd/include/hp/predictor.hpp` | the model to flatten in P1 |
| `compress/hpd/include/hp/models.hpp` | bucketed `ContextModel`, match models |
| `compress/hpd/include/hp/record.hpp` | record model |
| `compress/hpd/include/hp/mixer.hpp` | mixer + APM |
| `compress/hpd/tools/pc_shard_bench.sh` / `.ps1` | P0 runner (bash tested on Linux; PowerShell untested) |
| `paper/hp40/HPD_part2.md` | current CPU results to beat |
