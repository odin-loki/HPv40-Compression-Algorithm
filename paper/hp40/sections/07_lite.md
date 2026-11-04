## 7. Designing HP40-Lite from the data

### 7.1 Leave-one-family-out

Each of HP40's 20 switchable expert families was removed in turn and the
variant compressed the 13-file tuning set at `--mem 18`
(`data/abl_m18.jsonl`). HP40 itself writes 945,166 bytes for the set.
CPU deltas were measured with four jobs sharing the machine, so treat them as
±3 points.

| removed family | experts | Δ size | worst file | CPU | RSS |
|---|---:|---:|---|---:|---:|
| byte match order 8 (`m8`) | 1 | −0.001% | +0.05% nci | −1.0% | 683 MiB |
| link CM | 2 | +0.003% | +0.05% enwik8 | −2.8% | 685 MiB |
| numeric CM | 2 | +0.005% | +0.06% webster | −2.9% | 688 MiB |
| Hebbian | 1 | +0.005% | +0.04% xml | −0.4% | 689 MiB |
| byte match order 5 (`m5`) | 1 | +0.006% | +0.03% samba | −1.6% | 683 MiB |
| sentence-word CM | 2 | +0.015% | +0.07% enwik8 | −4.5% | 685 MiB |
| 4th word match | 1 | +0.016% | +0.11% nci | −2.7% | 688 MiB |
| word-stream CM | 2 | +0.028% | +0.14% osdb | −3.5% | 685 MiB |
| sentence memory | 2 | +0.037% | +0.12% nci | −6.3% | 688 MiB |
| all word matches | 4 | +0.064% | +0.43% nci | −5.1% | 684 MiB |
| tag CM | 2 | +0.087% | +0.60% xml | −5.5% | 685 MiB |
| sentence-stream CM | 2 | +0.093% | +0.61% nci | −5.5% | 681 MiB |
| sparse-UTF-8 match | 1 | +0.103% | +0.44% ooffice | −3.4% | 683 MiB |
| word-bigram CM (`wbi`) | 2 | +0.126% | +0.73% dickens | −9.4% | **433 MiB** |
| bracket CM | 2 | +0.144% | +0.62% enwik8 | −2.6% | 688 MiB |
| byte match orders 1+2 | 2 | +0.241% | +0.58% mozilla | −3.1% | 677 MiB |
| column CM | 2 | +0.247% | **+11.66% nci** | −4.2% | 689 MiB |
| sparse CMs sp13+sp24 | 4 | +0.418% | +2.09% ooffice | −7.9% | 688 MiB |
| discovery pool 12→4 slots | 16 | +1.219% | +3.27% sao | **−27.1%** | 685 MiB |
| discovery pool 12→1 slot | 22 | +1.561% | +4.24% sao | −37.4% | 683 MiB |


![Figure 11. Leave-one-family-out ablation on the tuning set: size cost, worst-file cost and CPU saving.](figures/ablation.png)

*Figure 11. Leave-one-family-out ablation on the tuning set: size cost, worst-file cost and CPU saving.*

Three things stand out.

1. **Most of HP40's experts are individually redundant.** Fourteen families
   (26 experts, a third of the mixer) each cost at most 0.15% when removed
   alone, and no file loses more than 0.73%. Nine of them cost under 0.04%.
2. **The column model is a specialist with no substitute.** It looks cheap on
   average (+0.25%) but carries the fixed-width chemistry table `nci`
   (+11.7% without it). An average-only selection rule would have deleted it.
3. **The discovery pool is the CPU budget.** Its 12 slots are 24 mixer inputs
   and about 27–37% of CPU time. It matters most on binary records: the star
   catalogue `sao` is the worst tuning file when it shrinks, and on
   `kennedy.xls` it is worth 22% on its own (§6.5).

### 7.2 Interactions: removals are not additive

| candidate | families dropped | experts left | Δ size (sum of singles) | Δ size (measured) | CPU | RSS |
|---|---|---:|---:|---:|---:|---:|
| L3 | 9 near-free | 61 | +0.114% | +0.127% | −15.2% | 663 MiB |
| L5 | L3 + wbi | 59 | +0.240% | +0.285% | −18.0% | 407 MiB |
| L1 | 14 cheap | 49 | +0.715% | +1.144% | −28.4% | 385 MiB |
| **L4 = HP40-Lite** | **L5 + discovery 12→4** | **43** | +1.459% | **+1.482%** | **−43.2%** | **403 MiB** |
| L2 | L1 + discovery 12→4 | 33 | +1.934% | +2.355% | −52.1% | 380 MiB |

The nine near-free families are nearly free together too (L3 measures +0.127% against the
+0.114% sum). Dropping all fourteen cheap families (L1) costs 60% more than the sum
of the singles: the word-oriented experts (`wbi`, word streams, sentence
word/stream, word matches, tag, bracket) cover for each other, so each looks
redundant only while the others are present.

A greedy **add-back** from L1 confirms this (`data/addback_m18.jsonl`): putting
any one family back recovers between 0.007% (`m8`) and 0.22% (bracket) of
HP40's size, and the best recoveries per expert are the sparse-UTF-8 match
(1,252 B per expert), bracket (1,053), tag (1,018), sentence stream (864) and
word-bigram (858). No single family explains L1's loss; it is spread across
the word cluster.

### 7.3 The chosen Lite

HP40-Lite is **L4**: drop the nine near-free families and the word-bigram
model, and cut the discovery pool from 12 slots to 4. It keeps every family
with a measured specialist role (column, sparse, bracket, tag, sentence
stream, sparse-UTF-8, three word matches, low-order byte matches).

```
-DHP_MATCH_18=0 -DHP_MATCH_05=0 -DHP_LINKWORD=0 -DHP_NUMERIC=0 -DHP_HEBB_EXPERT=0
-DHP_SENWORD=0 -DHP_WMATCH_4=0 -DHP_WORD_STREAMS=0 -DHP_SENT_MEM=0 -DHP_CM_WBI=0
-DHP_DISC_SLOTS=4
```

On the tuning set it is the best point we found on the size/CPU curve
(1.48% larger for 43% less CPU; the discovery pool alone buys 27% CPU for
1.22%, and adding the ten other drops buys a further 16 points of CPU and the
word-bigram table's memory for another 0.26%). L5 is the choice when size
matters more than speed: +0.29% for −41% memory. Both are evaluated on
the full GP set in §4 (Table 1).
