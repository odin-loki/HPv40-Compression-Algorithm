#!/usr/bin/env python3
"""analyze_abl.py RESULTS.jsonl : per-variant total size delta vs base, per-file deltas, time and RSS."""
import json, sys, collections, os
rows = [json.loads(l) for l in open(sys.argv[1])]
by = collections.defaultdict(dict)
for r in rows:
    v = r["codec"].split("/")[1].rsplit(":", 1)[0]
    by[v][os.path.basename(r["file"])] = r
base = by["base"]
files = sorted(base)
NEXP = {"no_sp": 4, "no_col": 2, "no_tag": 2, "no_wbi": 2, "no_hebb": 1, "no_wstr": 2, "no_brk": 2, "no_link": 2,
        "no_num": 2, "no_sen": 2, "no_sentst": 2, "no_sentmem": 2, "no_m12": 2, "no_m5": 1, "no_m8": 1,
        "no_sutf8": 1, "no_wm": 4, "no_wm4": 1, "disc4": 16, "disc1": 22, "base": 0}
out = []
bt = sum(base[f]["comp"] for f in files); bcpu = sum(base[f]["c"]["user"] + base[f]["c"]["sys"] for f in files)
for v, d in by.items():
    if set(d) != set(files): print("incomplete", v, len(d)); continue
    t = sum(d[f]["comp"] for f in files)
    cpu = sum(d[f]["c"]["user"] + d[f]["c"]["sys"] for f in files)
    rss = max(d[f]["c"]["maxrss_kb"] for f in files)
    per = {f: 100 * (d[f]["comp"] - base[f]["comp"]) / base[f]["comp"] for f in files}
    out.append(dict(variant=v, experts_removed=NEXP.get(v), total=t, dpct=100 * (t - bt) / bt,
                    worst_file=max(per, key=per.get), worst_dpct=max(per.values()), best_dpct=min(per.values()),
                    cpu_s=cpu, dcpu_pct=100 * (cpu - bcpu) / bcpu, rss_mb=rss / 1024, per_file=per))
out.sort(key=lambda r: r["dpct"])
for r in out:
    print("%-11s -%2s exp  total %8d  %+6.3f%%  worst %+6.2f%% (%s)  best %+6.2f%%  cpu %+6.1f%%  rss %5.0f MB" % (
        r["variant"], r["experts_removed"], r["total"], r["dpct"], r["worst_dpct"], r["worst_file"], r["best_dpct"], r["dcpu_pct"], r["rss_mb"]))
if len(sys.argv) > 2: json.dump(out, open(sys.argv[2], "w"), indent=1)
