#!/usr/bin/env python3
"""analyze_mine.py {v40|lite} OUT.json PREFIX... : summarise mine outputs (json + i16 records)."""
import json, sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from expert_names import names_for, family

def load(prefix):
    j = json.load(open(prefix + ".json"))
    n = j["n_experts"]
    raw = np.fromfile(prefix + ".i16", dtype=np.int16)
    rec = raw.reshape(-1, n + 2) if raw.size else np.zeros((0, n + 2), np.int16)
    return j, rec

def bits(st, y):
    p = 1 / (1 + np.exp(-st / 256.0))
    p = np.clip(p, 1 / 4096, 4095 / 4096)
    return -np.log2(np.where(y == 1, p, 1 - p))

def main():
    variant, out, prefixes = sys.argv[1], sys.argv[2], sys.argv[3:]
    res = {}
    for pre in prefixes:
        j, rec = load(pre)
        n = j["n_experts"]; nm = names_for(variant)
        assert len(nm) == n, (len(nm), n)
        B = j["bytes"]
        eb = np.array(j["expert_bits"]) / B   # bits per byte, standalone
        best = np.array(j["expert_best"]) / j["bits"]
        r = dict(bytes=B, final_bpc=j["final_bits"] / B, oracle_bpc=j["oracle_bits"] / B,
                 best_single=nm[int(eb.argmin())], best_single_bpc=float(eb.min()),
                 expert_bpc=dict(zip(nm, eb.round(4).tolist())),
                 best_share=dict(zip(nm, best.round(5).tolist())),
                 bitpos_bpc_share=(np.array(j["bitpos_bits"]) / j["final_bits"]).round(4).tolist(),
                 block=j["block"], block_bpc=(np.array(j["block_bits"]) / j["block"]).round(4).tolist())
        if len(rec) > 1000:
            X = rec[:, :n].astype(np.float64); y = rec[:, n + 1]
            C = np.corrcoef(X.T)
            C = np.nan_to_num(C)
            ev = np.linalg.eigvalsh(C)[::-1]; ev = np.clip(ev, 0, None)
            pr = ev.sum() ** 2 / (ev ** 2).sum()           # participation ratio (effective rank)
            k90 = int(np.searchsorted(np.cumsum(ev) / ev.sum(), 0.9) + 1)
            # most redundant pairs
            iu = np.triu_indices(n, 1)
            order = np.argsort(-np.abs(C[iu]))[:15]
            pairs = [(nm[iu[0][k]], nm[iu[1][k]], round(float(C[iu][k]), 3)) for k in order]
            # family-level effective rank
            r.update(eff_rank=float(pr), k90=k90, top_eig_share=float(ev[0] / ev.sum()),
                     top_pairs=pairs, n_records=int(len(rec)))
            # family mean |corr| with the final mixer output
            fin = rec[:, n].astype(np.float64)
            cf = [float(np.corrcoef(X[:, e], fin)[0, 1]) if X[:, e].std() > 0 else 0.0 for e in range(n)]
            r["corr_with_final"] = dict(zip(nm, np.round(cf, 3).tolist()))
        res[os.path.basename(pre)] = r
        print(os.path.basename(pre), "final %.4f oracle %.4f best %s %.4f" % (r["final_bpc"], r["oracle_bpc"], r["best_single"], r["best_single_bpc"]),
              "effrank %.1f k90 %s" % (r.get("eff_rank", -1), r.get("k90")))
    json.dump(res, open(out, "w"), indent=1)

if __name__ == "__main__":
    main()
