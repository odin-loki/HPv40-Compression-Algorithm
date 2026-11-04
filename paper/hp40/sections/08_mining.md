## 6. Data-mining the experts

The mining driver ran HP40 and HP40-Lite at `--mem 18` over one file of each
of 18 data types (the 12 Silesia-1M files, enwik8's first MiB, and Calgary
book1, geo, obj2, pic and Canterbury kennedy.xls) and logged every expert's
cost on every coded bit (`data/mine_v40.json`, `data/mine_lite.json`).
Calgary `pic` and Canterbury `ptt5` turned out to be the same file
(byte-identical, same results), so 18 distinct types remain.

### 6.1 Mixing is where the compression comes from

| data | HP40 bpc | best single expert | its bpc | mixing gain | effective rank | PCs for 90% | Lite Δ |
|---|---:|---|---:|---:|---:|---:|---:|
| book1 (novel) | 2.084 | o4 PY | 2.680 | 22% | 2.4 | 21 | +0.7% |
| dickens (novels) | 1.791 | o4 PY | 2.318 | 23% | 2.3 | 20 | +1.6% |
| enwik8 1 MiB | 1.746 | o4 PY | 2.560 | 32% | 2.5 | 24 | +0.7% |
| webster (dictionary) | 1.322 | o4 PY | 1.921 | 31% | 2.3 | 19 | +0.9% |
| reymont (PDF) | 1.231 | o4 PY | 1.718 | 28% | 2.2 | 18 | +0.9% |
| xml (tar) | 0.662 | o4 PY | 1.147 | 42% | 1.7 | 10 | +1.7% |
| nci (chem. table) | 0.336 | **col** PY | 0.794 | 58% | 1.6 | 9 | +1.1% |
| samba (tar of source) | 1.834 | o3 | 2.533 | 28% | 2.2 | 20 | +0.9% |
| mozilla (tar of binaries) | 4.761 | o3 | 5.061 | 6% | 2.0 | 16 | +0.3% |
| obj2 (object code) | 1.697 | o3 PY | 2.884 | 41% | 2.8 | 24 | +2.5% |
| ooffice (x86 DLL) | 3.151 | o3 PY | 4.512 | 30% | 4.2 | 28 | +2.7% |
| kennedy.xls | 0.163 | o3 | 0.580 | **72%** | 2.8 | 22 | **+33.5%** |
| osdb (database) | 1.979 | o3 | 2.705 | 27% | 3.7 | 26 | +1.9% |
| sao (star catalogue) | 4.711 | o2 | 5.913 | 20% | **7.2** | **36** | +3.7% |
| geo (seismic floats) | 3.695 | **sp24** PY | 4.444 | 17% | 4.2 | 28 | +1.4% |
| mr (MRI) | 1.639 | o2 | 1.865 | 12% | 3.4 | 18 | −0.1% |
| x-ray (12-bit image) | 3.428 | o2 | 4.056 | 15% | 3.4 | 27 | +0.2% |
| pic / ptt5 (fax) | 0.659 | o1 | 0.830 | 21% | 2.1 | 5 | −0.3% |

*Mixing gain* = 1 − HP40 ÷ best single expert in hindsight. *Effective rank*
is the participation ratio of the eigenvalues of the 75×75 correlation matrix
of expert logits (1 = all experts say the same thing, 75 = independent).
"PY" marks the Pitman–Yor direct output of a context model; unmarked is the
bit-history (indirect) output.

* **No single expert comes close.** The mix beats the best expert by 6–72%
  (median 27%). The best single expert is always a plain order-1 to order-4
  model (or the column / sparse model on tabular and float data), never one of
  the Wikipedia-specific experts.
* **The expert set is low-rank.** The participation ratio is 1.6–7.2 out of 75,
  and 90% of the variance sits in 5–36 principal components. On text it is
  about 2.3; it rises on the binary, structured data where HP40 is weakest
  (sao 7.2, ooffice 4.2, geo 4.2). The experts disagree most exactly where they
  are least useful.
* **The most redundant pairs are near-duplicates**: word matches 1 and 3
  (r = 0.99 on both text and code), byte matches of orders 8/10/16 (0.93–0.94),
  and adjacent orders' two outputs (o3/o4, o4/o6, 0.92–0.96). On ooffice the
  link and sentence-word models (both text features) correlate at 0.95: on
  binary data they collapse to the same degenerate context.


![Figure 7. Best single expert (in hindsight) against the mixed output, per data type.](figures/mixing_gain.png)

*Figure 7. Best single expert (in hindsight) against the mixed output, per data type.*

![Figure 8. Correlation of the 75 expert logits on text and on an x86 DLL, and the cumulative variance spectrum.](figures/correlation.png)

*Figure 8. Correlation of the 75 expert logits on text and on an x86 DLL, and the cumulative variance spectrum.*

### 6.2 Who wins which bits

Averaged over the 18 types, the share of bits on which each family's
prediction was the cheapest (`best_share`):

| family | share | family | share |
|---|---:|---|---:|
| order-3 CM | 15.8% | sparse sp24 / sp13 | 2.9% / 2.8% |
| order-1 CM | 15.5% | sentence stream | 2.1% |
| discovery pool (12 slots) | 14.3% | word CM | 1.9% |
| order-4 CM | 12.2% | bracket | 1.7% |
| order-2 CM | 10.7% | word bigram | 1.5% |
| order-6 CM | 7.3% | column, tag | 1.3% each |
| byte match (9 models) | 4.4% | sentence memory | 1.2% |
| | | word stream, sparse-UTF-8, numeric, word match, link, sentence word, Hebbian | ≤ 0.8% each |

The bit-history (indirect) and Pitman–Yor (direct) outputs of the context
models split the wins about evenly (49% / 45%), so the second output per model
is not wasted. The seven families at the bottom of the table win under 1% of
bits each; five of them are among the families HP40-Lite drops, and the
ablation (§7) agrees that they are close to free to remove.

Figure 9 is the per-type view. The ratio panel shows the specialists clearly:
`col` is nearly as good as the whole mix on `nci`, `sp24` is the best single
expert on `geo` (4-byte floats, so bytes 2 and 4 back are the same field), and
the word-keyed experts (word, wbi, word match, link, sentence) are 3–8× worse
than the mix on every binary file, yet the mixer still has to evaluate them on
every bit.


![Figure 9. Every expert on every data type: standalone cost relative to the mix (left) and share of bits won (right).](figures/expert_heatmap.png)

*Figure 9. Every expert on every data type: standalone cost relative to the mix (left) and share of bits won (right).*

### 6.3 Where the bits go inside a byte

Share of coded bits by bit position (MSB first), averaged over types:
**6.1, 10.5, 8.5, 15.0, 16.0, 16.6, 14.2, 13.1%**. The first three bits of a
byte cost a quarter of the output (they choose among character classes,
which the contexts predict well); the low five bits cost three quarters. This
is the usual shape for bitwise CM and suggests nothing specific to HP40.

### 6.4 Learning dynamics

Figure 10 shows 64 KiB blocks. Text and the chemistry table are stable from the
first block. The DLL fluctuates with section content. `mozilla`'s first MiB
contains roughly 640 KiB of already-compressed members, where HP40 codes at up
to **7.99 bits/byte**: it has no stored-block fallback, so it spends its full
per-bit cost (about 10 KB/s) on bytes it cannot shrink. A general-purpose tool
would detect such runs and store them.


![Figure 10. Bits per 64 KiB block across four 1 MiB inputs.](figures/learning_curves.png)

*Figure 10. Bits per 64 KiB block across four 1 MiB inputs.*

### 6.5 What the mining says about Lite

Lite's mixed output is within 3.7% of HP40 on 17 of 18 types and slightly
*better* on the two images (`mr`, fax). With fewer, less redundant experts its
90%-variance count drops from 5–36 to 5–23. The exception is **kennedy.xls**
(+33.5%, 0.163 → 0.218 bpc). Re-running each ablation on that file
(`data/kennedy_m18.jsonl`) isolates the cause: cutting the **discovery pool**
from 12 to 4 slots costs +22.3% on its own, while every other dropped family
costs at most +0.6% alone (+5.2% together). kennedy.xls is a spreadsheet of
fixed-size records; the pool's online mask search finds record-aligned
contexts that no hand-written model in HP40 provides, and four slots are not
enough to hold them. HP40 reaches 0.16 bpc there and xz -9e 0.40, so Lite
still beats every baseline. This is also the clearest evidence that the
discovery pool is HP40's general-purpose engine: it is what adapts to
structure the hand-written experts were not designed for.
