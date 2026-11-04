# hpd on the GPU: sharded context mixing — plan

**Status (2026-11-05):** P0–P3 are done and measured on an RTX 3090 (24 GB,
CUDA 12.4, clang 23.1) in WSL2 on OdinsPC. The GPU build (`make gpu`, `hpd -G`)
writes archives **byte-identical** to the CPU build and each decodes the other's.
P2 (thread per shard) ran at half the 64-thread CPU's speed; P3 (warp per shard,
the default) brings the GPU to **0.84–0.95× the CPU**: parity, not a win. Shards
small enough to fill the card cost 27–45% in ratio. Results: §9.
**Code today:** `compress/hpd/` (hpd 1.2.0, archive format 3: the CPU build, plus the GPU build `make gpu` / `hpd -G` from P1–P2; the model is unchanged from the format-2 build measured in Part II).
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
| 1 MiB | 64 | see §9 | | |
| 256 KiB | 256 | see §9 | | |

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
| **P1** flat state | refactor `hp::Predictor` into a POD state struct + `__host__ __device__` `predict(state, …)` / `update(state, …)` functions over flat arrays (no `std::vector`, `shared_ptr`, `thread_local`, `mmap` inside the model) | CPU `hpd` archives byte-identical to hpd 1.0.0 on the GP set; `make test` passes |
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

## 9. Results (P0–P2, measured)

### P0: what sharding costs (hpd 1.0.0, 64 MiB test bed, CPU, every round trip exact)

| shard size | tables | shards | archive bytes | bits/byte | vs 16 MiB | vs xz -9e |
|---:|---|---:|---:|---:|---:|---:|
| 16 MiB | auto | 4 | 11,029,625 | 1.315 | 0 | −17.4% |
| 4 MiB | auto | 16 | 11,335,708 | 1.351 | +2.8% | −15.1% |
| 1 MiB | auto | 64 | 11,889,096 | 1.417 | +7.8% | −10.9% |
| 1 MiB | `-M 18` | 64 | 12,037,628 | 1.435 | +9.1% | −9.8% |
| 256 KiB | auto | 256 | 12,749,391 | 1.520 | +15.6% | −4.5% |
| 256 KiB | `-M 18` | 256 | 12,801,619 | 1.526 | +16.1% | −4.1% |
| 64 KiB | `-M 18` | 1,024 | 13,984,756 | 1.667 | +26.8% | +4.8% |
| 16 KiB | `-M 18` | 4,096 | 15,970,901 | 1.904 | +44.8% | +19.6% |
| xz -9e (one stream) | | 1 | 13,349,436 | 1.591 | | 0 |

The decision rule (≤ 5% loss) gives shards of about 4 MiB. Shards small
enough to fill a GPU with one thread each (≤ 64 KiB) lose to xz -9e.

### P1: the model on the GPU

Done without rewriting the model. The model headers are compiled for host and
device with clang's `#pragma clang force_cuda_host_device` (`src/hpd.cpp`
under `-x cuda -DHPD_GPU`). The few things device code cannot do were replaced:
`include/hp/hd.hpp` gives `vec` / `shared` (aliases of `std::vector` /
`std::shared_ptr` in the CPU build, small host/device classes in the GPU build);
the `thread_local` slot cap became a Predictor member; `ZeroArray` and every
allocation take pre-zeroed memory from a per-shard arena; the four tables built
at start-up (stretch, log2, state, Pitman–Yor) are built on the CPU and copied
to the GPU once; AVX2 and prefetch stay host-only. The CPU build writes the
same bytes as hpd 1.0.0.

### P2: one GPU thread per shard (`src/hpd_gpu.cuh`)

The CPU prepares each block (CRC, x86 transform, pass-through chunks), the GPU
runs the model and arithmetic coder for every shard at once, the CPU finishes
the container. Kernels run in ~100 ms launches so the Windows display driver
does not reset the GPU. 64 MiB test bed, 16 KiB shards, `-M 18`
(table bits 16), RTX 3090 vs the 64-thread Xeon Gold 6242 in the same machine:

| | GPU (`hpd -G`) | CPU, 64 threads |
|---|---:|---:|
| archive | 15,970,901 B | 15,970,901 B (identical bytes) |
| compress | 114 s, 575 KiB/s | 56.5 s, 1,161 KiB/s |
| decompress | 114 s, 573 KiB/s | 60 s, 1,094 KiB/s |

* Model memory per shard is 19.3 MB even at the smallest tables, so the
  24 GB card holds 1,134 shards at a time: about 14 threads per SM, far too few
  to hide memory latency.
* One GPU thread runs the model at about 2 KiB/s (one CPU thread: 20–80 KiB/s).
* Threads per block: 1 → 0.6 MiB/s, 4 → 0.5, 16 → 0.4, 32 → 0.3. Shards in the
  same warp take different branches and different cache lines and stall each
  other, so warps of one thread are fastest.
* 64 MiB shards, as first asked: about 40 MB per shard, but a 64 MiB file is one
  shard, one GPU thread at ~2 KiB/s: about 9 hours. Large shards only help when
  the input is many GB.

### P3: one warp per shard (`include/hp/warp_ops.hpp`, default)

`hp::WarpOps` runs `Predictor::predict` / `update` with the 32 lanes of a warp:
every context model's lookup and indirect estimate on its own lane (13 main,
3 record, 4 discovery; `ContextModel::predict` is split into `predict_ind` and
`predict_py` so the Pitman–Yor backoff chain is one short dependent step), the
11 match models on 11 more lanes, the 11 layer-1 mixer dot products over all
lanes (exact int64 warp reductions), the hedge update one expert per lane, and
every model / mixer / APM update in parallel. Lane 0 keeps the short serial
parts (mixer gates, layer 2, APM refine, end-of-byte bookkeeping). All lanes
run the same arithmetic coder; lane 0 writes the output. Same bytes as the CPU.
`HPD_GPU_WARP=0` selects P2.

64 MiB test bed, RTX 3090 vs 64-thread Xeon Gold 6242, archives identical:

| | P2 GPU | **P3 GPU** | CPU, 64 threads |
|---|---:|---:|---:|
| 16 KiB shards, compress | 575 KiB/s | **940 KiB/s** | 1,069 KiB/s |
| 16 KiB shards, decompress | 573 KiB/s | **899 KiB/s** | 1,068 KiB/s |
| 64 KiB shards, compress | — | **1,091 KiB/s** | 1,153 KiB/s |
| 64 KiB shards, decompress | — | **1,078 KiB/s** | 1,194 KiB/s |

The machine was shared (load 8–55 during these runs; heavier load slows the
CPU more than the GPU, and with load 26–53 the GPU was ahead at 16 KiB shards,
890 against 549 KiB/s).

* One shard alone: 5.7 KiB/s (P2 thread: 2 KiB/s). With 1,024 shards each gets
  1 KiB/s: about 1.0 MiB/s for the card.
* Phase profile (clock64 per phase, 1,024 shards, `-DHPD_WARP_PROF`): mixer dots
  16%, mixer update 17%, model updates 17%, lookups and match 13%, PY 6%, lane-0
  serial parts (gates, layer 2 + hedge + APM, gria, end of byte) about 30%.
* Not bandwidth-bound: DRAM controller 34% busy, SMs 100% busy. The warp
  issues instructions for few useful lanes: lanes run different models (CM,
  match, hashing) and lane 0 runs serial code while 31 lanes wait.
* Tried and kept: hedge update over lanes (no measurable change). Tried and
  dropped: reading all 11 mixer rows in one pass (`HPD_WARP_UNROLL=1`):
  0.9 against 1.0 MiB/s, slower.
* Nsight Compute stall counters need "allow access to GPU performance counters"
  in the Windows NVIDIA settings (ERR_NVGPUCTRPERM); not available here.

### What is left

The card is issue-bound with mostly idle lanes, and memory (19.3 MB per shard)
caps it at ~1,134 shards. What could still win, all large changes:

1. **32 shards per warp in lock-step** (P2 layout) with ~30,000 shards resident,
   which needs per-shard state near 0.7 MB: a smaller model (the 4.3 MB APM,
   the match tables, int16 mixer weights). That changes the archive bytes, so it
   would be a new model setting, not hpd 1.x.
2. **Fewer 64-bit operations**: hashing (mix64), 64-bit products and divisions
   cost several GPU instructions each.
3. **Parallel end-of-byte**: the 11 match `push_byte` calls and context hashing.

Until then the CPU build is as fast and keeps large shards (better ratio).
