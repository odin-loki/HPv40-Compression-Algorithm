#!/usr/bin/env python3
"""make_hpd_figures.py — figures for Part II (hpd) from paper/hp40/data."""
import json, os, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DATA = os.path.join(ROOT, "data")
FIG = os.path.join(ROOT, "figures")
sys.path.insert(0, HERE)
from figstyle import setup, save, plt, CAT, HP40, LITE, OTHER, INK2  # noqa: E402

setup()
HPD = CAT[2]
H = "hpd:hpd:-t 1"

def rows(*names):
    out = {}
    for n in names:
        p = os.path.join(DATA, n)
        if not os.path.exists(p): continue
        for l in open(p):
            d = json.loads(l)
            out.setdefault(d["codec"], {})[d["file"]] = d
    return out

def corpus(f):
    for k, s in (("calgary", "cal"), ("cantrbry", "cant"), ("sil1m", "sil"), ("enwik8", "enwik8")):
        if "/corpora/" + k in f: return s
    return "?"

def fig_perfile():
    r = rows("gp_hpd_t1.jsonl", "gp_hp40_m22.jsonl", "base_gp.jsonl")
    files = [f for f in sorted(r["xz9e"]) if not f.endswith("/ptt5")]
    d40 = {f: 100 * (r[H][f]["comp"] / r["hp40"][f]["comp"] - 1) for f in files}
    dxz = {f: 100 * (r[H][f]["comp"] / r["xz9e"][f]["comp"] - 1) for f in files}
    order = sorted(files, key=lambda f: d40[f])
    lab = [os.path.basename(f) if corpus(f) == "enwik8" else "%s/%s" % (corpus(f), os.path.basename(f)) for f in order]
    fig, axs = plt.subplots(1, 2, figsize=(10.5, 8.4), sharey=True)
    y = np.arange(len(order))
    axs[0].barh(y, [d40[f] for f in order], color=[HPD if d40[f] < 0 else HP40 for f in order], height=0.7)
    axs[0].set_title("hpd vs HP40 (mem 22): archive size change (%)")
    axs[1].barh(y, [dxz[f] for f in order], color=HPD, height=0.7)
    axs[1].set_title("hpd vs xz -9e: archive size change (%)")
    axs[0].set_yticks(y, lab, fontsize=7.5); axs[0].invert_yaxis()
    for ax in axs: ax.axvline(0, color=INK2, lw=0.8); ax.grid(axis="y", visible=False)
    for i, f in enumerate(order):
        if d40[f] < -5: axs[0].text(d40[f] + 0.4, i, "%.1f%%" % d40[f], va="center", fontsize=7, color="white")
    fig.suptitle("hpd, single thread, on the 41 distinct GP files (negative = hpd smaller)", x=0.01, ha="left", fontweight="bold", fontsize=10.5)
    save(fig, os.path.join(FIG, "hpd_perfile.png"))

def fig_pareto():
    gp = rows("gp_hpd_t1.jsonl", "gp_hp40_m22.jsonl", "gp_lite_m22.jsonl", "gp_m18.jsonl", "base_gp.jsonl")
    ct = rows("clean_timing_v2.jsonl")
    files = sorted(gp["xz9e"])
    raw = sum(gp["xz9e"][f]["raw"] for f in files)
    pts = []
    names = {H: "hpd", "hp40": "HP40 mem 22", "hp40lite": "Lite mem 22", "hp40_m18": "HP40 mem 18",
             "hp40lite_m18": "Lite mem 18", "xz9e": "xz -9e", "brotli11": "brotli -q11", "zstd22u": "zstd -22",
             "bzip2-9": "bzip2 -9", "gzip9": "gzip -9"}
    for c, nm in names.items():
        if c not in gp or c not in ct: continue
        bpc = 8 * sum(gp[c][f]["comp"] for f in files) / raw
        cr = sum(x["raw"] for x in ct[c].values()); cw = sum(x["c"]["wall"] for x in ct[c].values())
        pts.append((nm, cr / 1024 / cw, bpc, c))
    fig, ax = plt.subplots(figsize=(7.4, 4.4))
    for nm, x, y, c in pts:
        col = HPD if c == H else (HP40 if c.startswith("hp40") and "lite" not in c else (LITE if "lite" in c else OTHER))
        ax.scatter(x, y, s=70 if c == H else 46, color=col, zorder=4, edgecolor="white", linewidth=1.5)
        off = {"HP40 mem 22": (-4, -14), "HP40 mem 18": (-30, 9), "Lite mem 22": (2, -14), "Lite mem 18": (6, 4), "hpd": (8, -3)}.get(nm, (6, 3))
        ax.annotate(nm, (x, y), xytext=off, textcoords="offset points", fontsize=8, color=INK2,
                    fontweight="bold" if c == H else "normal")
    ax.set_xscale("log")
    ax.set_xlabel("compression speed, KiB/s, single job alone (log; enwik8 1 MiB + ooffice + book1)")
    ax.set_ylabel("bits per byte, GP set (42 files)")
    ax.set_title("hpd is smaller and faster than every HP40 setting")
    ax.set_ylim(1.78, 2.98)
    save(fig, os.path.join(FIG, "hpd_pareto.png"))

if __name__ == "__main__":
    os.makedirs(FIG, exist_ok=True)
    fig_perfile(); fig_pareto()
    print("ok")
