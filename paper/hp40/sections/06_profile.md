## 5. Profiling

### 5.1 Where the CPU goes

**Optimised build** (`-O3`, `perf record -e cpu-clock`, `enwik8.1mb`,
`--mem 22`). The compiler inlines every model into two functions, so this
view separates only the codec from the kernel:

| symbol | HP40 | HP40-Lite |
|---|---:|---:|
| `Predictor::predict` (all experts + mixer + APM) | 52.1% | 63.0% |
| `Predictor::update` (all learning) | 15.0% | 21.0% |
| kernel `clear_page_erms` (zeroing the tables) | **23.6%** | 3.3% |
| `end_of_byte`, `memset`, page faults, other | 9.3% | 12.7% |

On a 1 MiB input, a quarter of HP40's CPU time at `--mem 22` is the kernel
zeroing 10.6 GiB of tables before and while they are first touched (§5.3).

**No-inline build** (`-O2 -fno-inline`, `--mem 18`, `enwik8.1mb`; shares are
of self time; disabling inlining inflates small helpers such as `clamp_int`
and `squash`, so read the ranking, not the absolute values):

| component | HP40 | Lite | HP40 on `ooffice` |
|---|---:|---:|---:|
| context models (`ContextModel`, `StateMap`, PY estimate) | **45.3%** | 40.0% | 41.5% |
| integer math (`log2_q16`, `squash`, `stretch`, `clamp`) | 19.8% | 20.3% | 21.3% |
| mixer (10 layer-1 dot products, layer 2, updates) | 19.6% | 21.8% | 21.8% |
| vector indexing, libc, other | 6.4% | 8.8% | 6.5% |
| predictor glue | 2.2% | 2.7% | 1.9% |
| byte match models | 2.2% | 2.8% | 2.3% |
| discovery pool (own code only) | 1.3% | 0.8% | 1.2% |
| APM ×3 | 1.0% | 1.1% | 1.2% |
| Hebbian, sentence memory, word match, text state machines, GRIA | 1.6% | 1.1% | 1.6% |
| arithmetic coder | 0.05% | 0.09% | 0.05% |

The profile is the same on text and on an x86 DLL: HP40 does the same work
per bit whatever the data.

![Figure 5. Self CPU time by component (no-inline build; shares only).](figures/cpu_profile.png)

*Figure 5. Self CPU time by component (no-inline build; shares only).*


**Call-graph view** (frame pointers, same input). Inclusive shares:
`Predictor::predict` 62.8%, `Predictor::update` 29.5%,
`ContextModel::predict` 35.9%, `DiscoveryPool::predict` **16.2%**,
`MixerNet::mix` 11.8%, `log2_q16` 11.7%, `MixerNet::update` 6.7%. The
discovery pool's 12 slots are `ContextModel`s, so most of their cost appears
under "context models" in the self-time table; inclusive, the pool's
predict + update is about 19% of HP40 and 9% of Lite. `log2_q16` is per-bit
loss bookkeeping (the Hedge layer over the 10 layer-1 mixers, the pool's
per-slot MDL scores, the GRIA entropy window). Shrinking the pool from 12 to
4 slots saved 27% of CPU in the ablation (§7), consistent with this.

### 5.2 The bottleneck is one DRAM load per model per bit

`perf annotate` on `ContextModel::predict` puts **95% of its samples on one
instruction**: the one retiring the load `state_ = t_[idx_]`, the bit-history
slot for this context. The index is a hash of the context and the partial
byte, so every lookup is a random access into a table of 4 MiB–4 GiB. HP40
does about 30 such lookups per coded bit (18 context models + 12 discovery
slots), 240 per byte. That load alone is about a third of total CPU time.
The coder (0.05%) and the APMs (1%) are irrelevant.

HP40 is therefore **memory-latency bound**, which explains three observations:

* speed falls as `--mem` grows (bigger tables, fewer cache hits) even
  after start-up is excluded;
* Lite's speed-up (1.8× in user CPU, Table 3) tracks its cut in random
  lookups per bit (30 → 16: 12 context models + 4 slots, 1.9×), not its cut in
  expert count (75 → 43, 1.7×);
* the classic CM remedy applies directly: keep the bit histories for a whole
  nibble in one 64-byte cache-line bucket, as paq8 and lpaq do, so a model
  costs about two cache misses per byte instead of eight. HP40 (v40) has one 2-byte slot per bit, with no
  bucket and no checksum (the lab added a checksummed 3-probe index,
  `HASH_CHK`, after v62).

### 5.3 Memory

Peak RSS is set by table sizes alone and is **independent of the input**: a
3,721-byte file and an 8 MiB file both use 10.6 GiB at `--mem 22`. The analytic
accounting in `scripts/memacct.py` reproduces the measured peaks within 4%
at `--mem 18–22`; at `--mem 16` a fixed ~20 MiB of code, mixer weights and
APM tables accounts for the gap (Figure 6):

| `--mem` | HP40 analytic | HP40 measured | Lite analytic | Lite measured |
|---:|---:|---:|---:|---:|
| 16 | 175 MiB | 195 MiB | 102 MiB | 123 MiB |
| 18 | 698 MiB | 704 MiB | 408 MiB | 418 MiB |
| 20 | 2,794 MiB | 2.7 GiB | 1,630 MiB | 1,598 MiB |
| 22 | 11,176 MiB | 10.6 GiB | 6,520 MiB | 6.2 GiB |

At `--mem 22`, 73% of HP40's memory is two tables, word and word-bigram
(2³¹ two-byte slots each, from the eight `SLOT_WORD` switches); the 14 match
models' private 64 MiB history buffers are another 0.9 GiB. Lite's saving is
almost entirely the word-bigram table.

![Figure 6. Table memory at `--mem 22` by component, HP40 and Lite (analytic).](figures/memory_map.png)

*Figure 6. Table memory at `--mem 22` by component, HP40 and Lite (analytic).*

**Start-up.** Every table is a value-initialised `std::vector`, so all of it is
written before the first byte is coded. On this VM that costs, for a
3.7 KB input at `--mem 22`, **36 s for HP40** (33.7 s of it kernel time) and
**5 s for Lite**. The cost is not linear in size. A first-touch micro-benchmark
(`scripts/touch.cpp`, `data/first_touch.txt`) zeroes fresh memory at
1.35 GiB/s up to 6 GiB but only 0.4–0.5 GiB/s beyond about 7 GiB, while
rewriting already-mapped memory runs at 4.3 GiB/s. The slow regime is the
hypervisor backing guest pages on first touch (the guest kernel is a
Firecracker build). On a host with ample free RAM, expect HP40's start-up to be a
few seconds. The structural point stands on any machine: the cost is paid in
full for every file, however small.
