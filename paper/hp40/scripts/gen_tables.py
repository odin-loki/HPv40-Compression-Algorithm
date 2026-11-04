#!/usr/bin/env python3
"""gen_tables.py — markdown tables for the paper, straight from data/*.jsonl. Writes data/tables.md."""
import collections, json, os

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(os.path.dirname(HERE), "data")

def jl(n):
    p = os.path.join(DATA, n)
    return [json.loads(l) for l in open(p)] if os.path.exists(p) else []

rows = (jl("base_gp.jsonl") + jl("gp_hp40_m22.jsonl") + jl("gp_lite_m22.jsonl") + jl("gp_m16.jsonl")
        + jl("gp_m18.jsonl") + jl("gp_m20.jsonl") + jl("gp_lite5_m18.jsonl"))
NAME = {"hp40": "HP40 (mem 22)", "hp40lite": "HP40-Lite (mem 22)", "hp40_m20": "HP40 mem 20", "hp40lite_m20": "Lite mem 20",
        "hp40_m18": "HP40 mem 18", "hp40lite_m18": "Lite mem 18", "hp40_m16": "HP40 mem 16", "hp40lite_m16": "Lite mem 16",
        "hp:hp_v40lite5:18": "Lite-L5 mem 18", "xz9e": "xz -9e", "brotli11": "brotli -q11 -w24", "bzip2-9": "bzip2 -9",
        "zstd22u": "zstd --ultra -22 --long", "zstd19": "zstd -19 --long", "gzip9": "gzip -9", "lz4hc": "lz4 -12"}
ORDER = list(NAME)
CORP = [("calgary", "Calgary"), ("cantrbry", "Canterbury"), ("sil1m", "Silesia-1M"), ("enwik8.8mb", "enwik8.8mb")]

def corpus(f):
    for k, _ in CORP:
        if "/corpora/" + k in f: return k
    return "?"

by = collections.defaultdict(dict)
for r in rows: by[r["codec"]][r["file"]] = r
files = sorted(by["xz9e"])
codecs = [c for c in ORDER if c in by and all(f in by[c] for f in files)]
out = []

# Table A: totals per corpus (bytes and bpc)
out.append("### Table A — archive bytes by corpus (bits per byte in brackets)\n")
out.append("| compressor | " + " | ".join(n for _, n in CORP) + " | **GP total** | vs xz -9e |")
out.append("|---|" + "---:|" * (len(CORP) + 2))
xzt = sum(by["xz9e"][f]["comp"] for f in files)
for c in codecs:
    cells = []
    for k, _ in CORP:
        fs = [f for f in files if corpus(f) == k]
        raw = sum(by[c][f]["raw"] for f in fs); comp = sum(by[c][f]["comp"] for f in fs)
        cells.append("%s (%.3f)" % (format(comp, ","), 8 * comp / raw))
    raw = sum(by[c][f]["raw"] for f in files); comp = sum(by[c][f]["comp"] for f in files)
    out.append("| %s | %s | **%s (%.3f)** | %+.1f%% |" % (NAME[c], " | ".join(cells), format(comp, ","), 8 * comp / raw, 100 * (comp / xzt - 1)))

# Table B: speed / memory
out.append("\n### Table B — time and memory on the GP set (42 files, 27,033,797 bytes)\n")
out.append("| compressor | compress wall | KiB/s | decompress wall | peak RSS | round trip |")
out.append("|---|---:|---:|---:|---:|---|")
for c in codecs:
    w = sum(by[c][f]["c"]["wall"] for f in files); raw = sum(by[c][f]["raw"] for f in files)
    rss = max(by[c][f]["c"]["maxrss_kb"] for f in files) / 1024
    dec = [by[c][f]["d"]["wall"] for f in files if "d" in by[c][f]]
    rt = [by[c][f].get("rt_ok") for f in files if "d" in by[c][f]]
    dw = ("%.1f s" % sum(dec) if sum(dec) < 600 else "%.1f min" % (sum(dec) / 60)) if len(dec) == len(files) else "—"
    rts = ("all %d OK" % len(rt) if all(rt) else "FAIL") if len(rt) == len(files) else "—"
    ws = "%.1f s" % w if w < 600 else "%.1f min" % (w / 60)
    out.append("| %s | %s | %.1f | %s | %s | %s |" % (NAME[c], ws, raw / 1024 / w,
               dw, ("%.0f MiB" % rss) if rss < 2048 else ("%.1f GiB" % (rss / 1024)), rts))

# Table C: per file
show = [c for c in ("hp40", "hp40lite", "xz9e", "brotli11", "bzip2-9", "zstd22u") if c in codecs]
out.append("\n### Table C — per file, archive bytes\n")
out.append("| file | bytes | " + " | ".join(NAME[c] for c in show) + " | HP40 vs xz |")
out.append("|---|---:|" + "---:|" * (len(show) + 1))
for f in sorted(files, key=lambda f: (corpus(f), os.path.basename(f))):
    best = min(by[c][f]["comp"] for c in show)
    cells = [("**%s**" if by[c][f]["comp"] == best else "%s") % format(by[c][f]["comp"], ",") for c in show]
    out.append("| %s/%s | %s | %s | %+.1f%% |" % ({"calgary": "cal", "cantrbry": "cant", "sil1m": "sil", "enwik8.8mb": "enwik"}[corpus(f)],
               os.path.basename(f), format(by["xz9e"][f]["raw"], ","), " | ".join(cells), 100 * (by["hp40"][f]["comp"] / by["xz9e"][f]["comp"] - 1)))

open(os.path.join(DATA, "tables.md"), "w").write("\n".join(out) + "\n")
print("\n".join(out[:20]))
