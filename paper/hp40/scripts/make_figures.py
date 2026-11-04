#!/usr/bin/env python3
"""make_figures.py — build every figure and the derived tables for the HP40 paper from paper/hp40/data."""
import collections, json, math, os, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DATA = os.path.join(ROOT, "data")
FIG = os.path.join(ROOT, "figures")
sys.path.insert(0, HERE)
from figstyle import setup, save, plt, CAT, SEQ, HP40, LITE, OTHER, INK, INK2, GRID  # noqa: E402
import memacct  # noqa: E402

setup()

def jl(name):
    p = os.path.join(DATA, name)
    return [json.loads(l) for l in open(p)] if os.path.exists(p) else []

def corpus_of(path):
    for k in ("calgary", "cantrbry", "sil1m", "enwik8"):
        if "/" + k in path or os.path.basename(path).startswith(k): return k
    return "other"

CORPUS_LABEL = {"calgary": "Calgary", "cantrbry": "Canterbury", "sil1m": "Silesia-1M", "enwik8": "enwik8.8mb"}

def by_codec(rows):
    d = collections.defaultdict(dict)
    for r in rows:
        d[r["codec"]][r["file"]] = r
    return d

# ---------------------------------------------------------------- figure 1: memory map
def fig_memory():
    a = memacct.components(22, "hp40"); b = memacct.components(22, "lite")
    def fam(k):
        return k.split(" x")[0]
    fa = collections.Counter(); fb = collections.Counter()
    for k, v in a.items(): fa[fam(k)] += v
    for k, v in b.items(): fb[fam(k)] += v
    keys = sorted(fa, key=lambda k: -fa[k])
    big = [k for k in keys if fa[k] >= 60 * 2**20]
    rest_a = sum(fa[k] for k in keys if k not in big); rest_b = sum(fb[k] for k in keys if k not in big)
    labels = big + ["everything else"]
    va = [fa[k] / 2**30 for k in big] + [rest_a / 2**30]
    vb = [fb.get(k, 0) / 2**30 for k in big] + [rest_b / 2**30]
    fig, ax = plt.subplots(figsize=(7.2, 3.9))
    y = np.arange(len(labels))
    ax.barh(y - 0.2, va, 0.38, color=HP40, label="HP40 (%.1f GiB total)" % (sum(fa.values()) / 2**30))
    ax.barh(y + 0.2, vb, 0.38, color=LITE, label="HP40-Lite (%.1f GiB total)" % (sum(fb.values()) / 2**30))
    ax.set_yticks(y, labels); ax.invert_yaxis()
    ax.set_xlabel("GiB at --mem 22 (analytic table sizes; measured peak RSS is within 4%)")
    for i, v in enumerate(va):
        ax.text(v + 0.05, i - 0.2, "%.2f" % v, va="center", fontsize=7.5, color=INK2)
    ax.set_title("Where HP40's memory goes: two 2³¹-slot word tables are 73% of it")
    ax.legend(loc="lower right"); ax.grid(axis="y", visible=False)
    save(fig, os.path.join(FIG, "memory_map.png"))

# ---------------------------------------------------------------- figure 5: ablation
def fig_ablation():
    p = os.path.join(DATA, "ablation_summary.json")
    if not os.path.exists(p): return
    rows = [r for r in json.load(open(p)) if not r["variant"].startswith(("lite", "base"))]
    nice = {"no_m8": "match o8", "no_link": "link CM", "no_num": "numeric CM", "no_hebb": "Hebbian",
            "no_m5": "match o5", "no_sen": "sentence-word CM", "no_wm4": "4th word match", "no_wstr": "word-stream CM",
            "no_sentmem": "sentence memory", "no_wm": "all word matches", "no_tag": "tag CM", "no_sentst": "sentence-stream CM",
            "no_sutf8": "sparse-UTF-8 match", "no_wbi": "word-bigram CM", "no_brk": "bracket CM", "no_m12": "match o1+o2",
            "no_col": "column CM", "no_sp": "sparse CMs", "disc4": "discovery 12→4", "disc1": "discovery 12→1"}
    lite = {"no_m8", "no_link", "no_num", "no_hebb", "no_m5", "no_sen", "no_wm4", "no_wstr", "no_sentmem", "no_wbi", "disc4"}
    rows.sort(key=lambda r: r["dpct"])
    lab = [nice.get(r["variant"], r["variant"]) for r in rows]
    col = [LITE if r["variant"] in lite else (OTHER if r["variant"] == "disc1" else HP40) for r in rows]
    fig, axs = plt.subplots(1, 3, figsize=(10.5, 5.2), sharey=True, gridspec_kw={"width_ratios": [1.2, 1.2, 1]})
    y = np.arange(len(rows))
    axs[0].barh(y, [r["dpct"] for r in rows], color=col, height=0.7)
    axs[0].set_title("Δ size, tuning set total (%)")
    axs[1].barh(y, [r["worst_dpct"] for r in rows], color=col, height=0.7)
    axs[1].set_title("Δ size, worst single file (%)")
    for i, r in enumerate(rows):
        if r["worst_dpct"] > 2:
            axs[1].text(min(r["worst_dpct"], 11.5) - 0.2, i, r["worst_file"].replace(".t", ""), ha="right", va="center", fontsize=7, color="white")
    axs[2].barh(y, [r["dcpu_pct"] for r in rows], color=col, height=0.7)
    axs[2].set_title("Δ CPU time (%)")
    axs[0].set_yticks(y, lab); axs[0].invert_yaxis()
    for ax in axs: ax.grid(axis="y", visible=False); ax.axvline(0, color=INK2, lw=0.8)
    from matplotlib.patches import Patch
    axs[0].legend(handles=[Patch(color=LITE, label="dropped in HP40-Lite"), Patch(color=HP40, label="kept in HP40-Lite"),
                           Patch(color=OTHER, label="alternative not used")], loc="upper right", bbox_to_anchor=(1.0, 0.82))
    fig.suptitle("Leave-one-family-out ablation of HP40 (--mem 18, 13-file tuning set)", x=0.01, ha="left", fontweight="bold", fontsize=10.5)
    save(fig, os.path.join(FIG, "ablation.png"))

# ---------------------------------------------------------------- GP tables
def gp_tables():
    base = jl("base_gp.jsonl")
    hp = jl("gp_hp40_m22.jsonl") + jl("gp_lite_m22.jsonl") + jl("gp_m18.jsonl") + jl("gp_m16.jsonl") + jl("gp_m20.jsonl")
    rows = base + hp
    d = by_codec(rows)
    files = sorted({r["file"] for r in base})
    out = {}
    for c, m in d.items():
        if not all(f in m for f in files): continue
        agg = collections.defaultdict(lambda: [0, 0, 0.0, 0.0, 0.0, 0])
        for f in files:
            r = m[f]
            for key in (corpus_of(f), "ALL"):
                a = agg[key]; a[0] += r["raw"]; a[1] += r["comp"]; a[2] += r["c"]["wall"]
                a[3] += r["c"]["user"] + r["c"]["sys"]; a[4] = max(a[4], r["c"]["maxrss_kb"]); a[5] += 1
        out[c] = {k: dict(raw=a[0], comp=a[1], bpc=8 * a[1] / a[0], wall=a[2], cpu=a[3], rss_mb=a[4] / 1024,
                          kbps=a[0] / 1024 / a[2], n=a[5]) for k, a in agg.items()}
        dec = [m[f]["d"]["wall"] for f in files if "d" in m[f]]
        if len(dec) == len(files):
            out[c]["ALL"]["dwall"] = sum(dec)
            out[c]["ALL"]["rt_ok"] = all(m[f].get("rt_ok") for f in files)
    json.dump(out, open(os.path.join(DATA, "gp_summary.json"), "w"), indent=1)
    return out, d, files

# ---------------------------------------------------------------- figure 2: pareto
def fig_pareto(summ):
    pts = {k: v["ALL"] for k, v in summ.items()}
    fig, (ax, az) = plt.subplots(1, 2, figsize=(10.5, 4.3), gridspec_kw={"width_ratios": [1.25, 1]})
    for k, v in pts.items():
        if k.startswith("hp"): continue
        ax.scatter(v["kbps"], v["bpc"], s=46, color=OTHER, zorder=3, edgecolor="white", linewidth=1.5)
        ax.annotate(k, (v["kbps"], v["bpc"]), xytext=(6, 3), textcoords="offset points", fontsize=8, color=INK2)
    series = (("HP40", HP40, ["hp40", "hp40_m20", "hp40_m18", "hp40_m16"], (6, 4)),
              ("HP40-Lite", LITE, ["hp40lite", "hp40lite_m20", "hp40lite_m18", "hp40lite_m16"], (6, -11)))
    for name, col, keys, off in series:
        ks = [k for k in keys if k in pts]
        if not ks: continue
        xs = [pts[k]["kbps"] for k in ks]; ys = [pts[k]["bpc"] for k in ks]
        for a in (ax, az):
            a.plot(xs, ys, color=col, zorder=4, label=name)
            a.scatter(xs, ys, s=46, color=col, zorder=5, edgecolor="white", linewidth=1.5)
        for k, x, y in zip(ks, xs, ys):
            m = "22" if k in ("hp40", "hp40lite") else k.split("_m")[1]
            az.annotate("mem " + m, (x, y), xytext=off, textcoords="offset points", fontsize=7.5, color=col)
    ax.set_xscale("log")
    ax.set_xlabel("compression speed, KiB/s (log scale)")
    ax.set_ylabel("bits per byte (GP set total, lower is better)")
    ax.set_title("(a) all compressors")
    ax.legend(loc="center right")
    az.set_xlabel("compression speed, KiB/s")
    az.set_title("(b) zoom: HP40 and Lite across --mem")
    az.set_xlim(6, 36)
    fig.suptitle("Ratio vs speed on the 27 MB general-purpose set (speed includes start-up; HP runs shared the machine)",
                 x=0.01, ha="left", fontweight="bold", fontsize=10)
    save(fig, os.path.join(FIG, "pareto.png"))

# ---------------------------------------------------------------- figure 3: per file vs xz
def fig_perfile(d, files):
    if "hp40" not in d or "xz9e" not in d: return
    order = sorted(files, key=lambda f: (corpus_of(f), os.path.basename(f)))
    SHORT = {"calgary": "cal", "cantrbry": "cant", "sil1m": "sil", "enwik8": "enwik8"}
    lab = [os.path.basename(f) if corpus_of(f) == "enwik8" else "%s/%s" % (SHORT[corpus_of(f)], os.path.basename(f)) for f in order]
    def gain(c):
        return [100 * (1 - d[c][f]["comp"] / d["xz9e"][f]["comp"]) for f in order]
    fig, ax = plt.subplots(figsize=(7.2, 8.6))
    y = np.arange(len(order))
    ax.barh(y - 0.2, gain("hp40"), 0.38, color=HP40, label="HP40 (mem 22)")
    if "hp40lite" in d and all(f in d["hp40lite"] for f in order):
        ax.barh(y + 0.2, gain("hp40lite"), 0.38, color=LITE, label="HP40-Lite (mem 22)")
    ax.set_yticks(y, lab, fontsize=7.5); ax.invert_yaxis()
    ax.axvline(0, color=INK2, lw=0.8)
    ax.set_xlabel("archive size saving vs xz -9e (%)")
    ax.set_title("Per file: how much smaller than xz -9e")
    ax.legend(loc="lower right"); ax.grid(axis="y", visible=False)
    save(fig, os.path.join(FIG, "perfile_vs_xz.png"))

# ---------------------------------------------------------------- figure 4: memory sweep
def fig_memsweep(summ):
    mems = [16, 18, 20, 22]
    def key(v, m):
        if m == 22: return "hp40" if v == "hp40" else "hp40lite"
        return ("hp40_m%d" if v == "hp40" else "hp40lite_m%d") % m
    fig, axs = plt.subplots(1, 3, figsize=(10.5, 3.3))
    for v, col, lab in (("hp40", HP40, "HP40"), ("lite", LITE, "HP40-Lite")):
        ms = [m for m in mems if key(v, m) in summ]
        if not ms: continue
        axs[0].plot(ms, [summ[key(v, m)]["ALL"]["bpc"] for m in ms], marker="o", color=col, label=lab)
        axs[1].plot(ms, [summ[key(v, m)]["ALL"]["rss_mb"] / 1024 for m in ms], marker="o", color=col, label=lab)
        axs[2].plot(ms, [summ[key(v, m)]["ALL"]["wall"] / 60 for m in ms], marker="o", color=col, label=lab)
    axs[0].set_title("bits per byte (GP set)"); axs[1].set_title("peak RSS (GiB, log)"); axs[2].set_title("compress wall time (min, GP set)*")
    axs[1].set_yscale("log")
    for ax in axs: ax.set_xlabel("--mem (table bits)"); ax.set_xticks(mems)
    axs[0].legend()
    fig.text(0.99, 0.01, "*runs shared the 4-core VM with 2–3 other jobs", ha="right", fontsize=7, color=INK2)
    save(fig, os.path.join(FIG, "mem_sweep.png"))

# ---------------------------------------------------------------- figure 10: startup / size scaling
def fig_startup(d, files):
    fig, ax = plt.subplots(figsize=(7.2, 4.0))
    for c, col, lab in (("hp40", HP40, "HP40 mem 22"), ("hp40lite", LITE, "HP40-Lite mem 22"),
                        ("hp40_m18", SEQ[2], "HP40 mem 18"), ("xz9e", OTHER, "xz -9e")):
        if c not in d: continue
        xs = [d[c][f]["raw"] for f in files if f in d[c]]; ys = [d[c][f]["c"]["wall"] for f in files if f in d[c]]
        ax.scatter(xs, ys, s=22, color=col, label=lab, edgecolor="white", linewidth=0.8, zorder=3)
    ax.set_xscale("log"); ax.set_yscale("log")
    ax.set_xlabel("input size (bytes, log)"); ax.set_ylabel("compress wall time (s, log)")
    ax.set_title("A fixed start-up cost dominates small files at --mem 22")
    ax.legend(loc="upper left")
    save(fig, os.path.join(FIG, "time_vs_size.png"))

if __name__ == "__main__":
    os.makedirs(FIG, exist_ok=True)
    fig_memory(); fig_ablation()
    summ, d, files = gp_tables()
    fig_pareto(summ); fig_perfile(d, files); fig_memsweep(summ); fig_startup(d, files)
    print("codecs complete on GP set:", sorted(summ))

# ================================================================ expert mining figures
MINE_ORDER = ["book1", "dickens", "enwik8.1mb", "webster", "reymont", "xml", "nci", "samba", "mozilla",
              "obj2", "ooffice", "kennedy.xls", "osdb", "sao", "geo", "mr", "x-ray", "pic"]
MINE_LABEL = {"book1": "book1 (novel)", "dickens": "dickens (novels)", "enwik8.1mb": "enwik8 1 MiB (wiki XML)",
              "webster": "webster (dictionary)", "reymont": "reymont (PDF, Polish)", "xml": "xml (tar of XML)",
              "nci": "nci (chem. table)", "samba": "samba (tar of src)", "mozilla": "mozilla (tar of binaries)",
              "obj2": "obj2 (Mac object code)", "ooffice": "ooffice (x86 DLL)", "kennedy.xls": "kennedy.xls (Excel)",
              "osdb": "osdb (database)", "sao": "sao (star catalogue)", "geo": "geo (seismic floats)",
              "mr": "mr (MRI image)", "x-ray": "x-ray (12-bit image)", "pic": "pic / ptt5 (fax bitmap)"}

def mine_summary(variant):
    p = os.path.join(DATA, "mine_%s.json" % variant)
    return json.load(open(p)) if os.path.exists(p) else {}

def fig_expert_heatmap():
    from expert_names import names_for
    s = mine_summary("v40")
    if not s: return
    files = [f for f in MINE_ORDER if ("v40_" + f) in s]
    nm = names_for("v40")
    # standalone cost of each expert relative to the mixed output, per file: (expert_bpc / final_bpc)
    M = np.array([[s["v40_" + f]["expert_bpc"][e] / s["v40_" + f]["final_bpc"] for f in files] for e in nm])
    B = np.array([[s["v40_" + f]["best_share"][e] for f in files] for e in nm])
    fig, axs = plt.subplots(1, 2, figsize=(11, 12.5), sharey=True)
    from matplotlib.colors import LinearSegmentedColormap, LogNorm
    seq = LinearSegmentedColormap.from_list("seq", SEQ[::-1] + ["#f4f3f0"])
    im = axs[0].imshow(np.clip(M, 1, 8), aspect="auto", cmap=seq, norm=LogNorm(1, 8))
    axs[0].set_title("Expert alone ÷ full mix (bits/byte ratio; 1 = as good as the mix)")
    cb = fig.colorbar(im, ax=axs[0], fraction=0.035, pad=0.02); cb.ax.minorticks_off(); cb.set_ticks([1, 1.5, 2, 3, 5, 8]); cb.set_ticklabels(["1", "1.5", "2", "3", "5", "≥8"])
    seq2 = LinearSegmentedColormap.from_list("seq2", ["#f4f3f0"] + SEQ)
    im2 = axs[1].imshow(B * 100, aspect="auto", cmap=seq2, vmin=0, vmax=max(10, float(np.percentile(B * 100, 99.5))))
    axs[1].set_title("Share of bits where this expert was the cheapest (%)")
    fig.colorbar(im2, ax=axs[1], fraction=0.035, pad=0.02)
    axs[0].set_yticks(range(len(nm)), nm, fontsize=6.5)
    for ax in axs:
        ax.set_xticks(range(len(files)), [MINE_LABEL[f] for f in files], rotation=60, ha="right", fontsize=7)
        ax.grid(False)
    fig.suptitle("HP40's 75 experts across 18 data types (--mem 18, every coded bit)", x=0.01, ha="left", fontweight="bold")
    save(fig, os.path.join(FIG, "expert_heatmap.png"))

def fig_mixing_gain():
    s = mine_summary("v40"); t = mine_summary("lite")
    if not s: return
    files = [f for f in MINE_ORDER if ("v40_" + f) in s]
    fig, ax = plt.subplots(figsize=(7.2, 4.6))
    y = np.arange(len(files))
    best = [s["v40_" + f]["best_single_bpc"] for f in files]
    fin = [s["v40_" + f]["final_bpc"] for f in files]
    ax.scatter(best, y, s=40, color=OTHER, label="best single expert (in hindsight)", zorder=3, edgecolor="white")
    ax.scatter(fin, y, s=40, color=HP40, label="HP40 mixed output", zorder=4, edgecolor="white")
    if t:
        ft = [t["lite_" + f]["final_bpc"] if ("lite_" + f) in t else np.nan for f in files]
        ax.scatter(ft, y, s=26, color=LITE, marker="D", label="HP40-Lite mixed output", zorder=5, edgecolor="white")
    for i in range(len(files)):
        ax.plot([fin[i], best[i]], [i, i], color=GRID, lw=2, zorder=2)
    ax.set_yticks(y, [MINE_LABEL[f] for f in files], fontsize=7.5); ax.invert_yaxis()
    ax.set_xlabel("bits per byte (--mem 18)")
    ax.set_title("Mixing beats the best single expert on every data type")
    ax.legend(loc="lower right", fontsize=7.5); ax.grid(axis="y", visible=False)
    save(fig, os.path.join(FIG, "mixing_gain.png"))

def fig_corr():
    from expert_names import names_for
    pairs = [f for f in ("enwik8.1mb", "ooffice") if os.path.exists(os.path.join(DATA, "corr_v40_%s.json" % f))]
    if not pairs: return
    nm = names_for("v40"); n = len(nm)
    fig, axs = plt.subplots(1, len(pairs) + 1, figsize=(4.2 * (len(pairs) + 1), 4.2))
    for ax, f in zip(axs, pairs):
        C = np.array(json.load(open(os.path.join(DATA, "corr_v40_%s.json" % f))))
        im = ax.imshow(C, cmap="RdBu_r", vmin=-1, vmax=1)
        ax.set_title("expert correlation: " + MINE_LABEL[f].split(" (")[0])
        ticks = [nm.index(k) for k in ("o1.a", "word.a", "sp13.a", "col.a", "wbi.a", "brk.a", "sentst.a", "m3", "wm1", "disc0.a")]
        ax.set_xticks(ticks, [nm[i].split(".")[0] for i in ticks], rotation=90, fontsize=6.5)
        ax.set_yticks(ticks, [nm[i].split(".")[0] for i in ticks], fontsize=6.5); ax.grid(False)
        ev = np.clip(np.linalg.eigvalsh(C)[::-1], 0, None)
        axs[-1].plot(np.arange(1, n + 1), np.cumsum(ev) / ev.sum(), label=MINE_LABEL[f].split(" (")[0],
                     color=HP40 if f == "enwik8.1mb" else CAT[2])
    fig.colorbar(im, ax=axs[len(pairs) - 1], fraction=0.045, pad=0.03)
    axs[-1].axhline(0.9, color=INK2, lw=0.8, ls="--")
    axs[-1].set_xlabel("principal components"); axs[-1].set_title("cumulative variance of 75 expert logits")
    axs[-1].legend(loc="lower right")
    save(fig, os.path.join(FIG, "correlation.png"))

def fig_learning():
    s = mine_summary("v40")
    if not s: return
    fig, ax = plt.subplots(figsize=(8.4, 3.8))
    picks = [("enwik8.1mb", CAT[0]), ("ooffice", CAT[1]), ("nci", CAT[2]), ("mozilla", CAT[3])]
    for f, col in picks:
        k = "v40_" + f
        if k not in s: continue
        b = s[k]["block_bpc"]
        ax.plot(np.arange(1, len(b) + 1) * 64, b, color=col, label=MINE_LABEL[f])
    ax.set_xlabel("position in file (KiB, 64 KiB blocks)"); ax.set_ylabel("bits per byte in block")
    ax.axhline(8, color=INK2, lw=0.8, ls="--"); ax.text(1030, 8.1, "8 bits/byte = stored", ha="right", fontsize=7.5, color=INK2)
    ax.set_ylim(0, 8.6)
    ax.set_title("Bits per 64 KiB block across four 1 MiB inputs (HP40, --mem 18)")
    ax.legend(loc="upper left", bbox_to_anchor=(1.0, 1.0), fontsize=7.5)
    save(fig, os.path.join(FIG, "learning_curves.png"))

if __name__ == "__main__":
    fig_expert_heatmap(); fig_mixing_gain(); fig_corr(); fig_learning()

def fig_cpu():
    a = os.path.join(DATA, "v40_ni_enwik.json"); b = os.path.join(DATA, "lite_ni_enwik.json")
    if not (os.path.exists(a) and os.path.exists(b)): return
    ca = json.load(open(a))["components"]; cb = json.load(open(b))["components"]
    short = {"context models (hash CMs)": "context models", "squash/stretch/log math": "integer math (log2, squash, clamp)",
             "mixer (layer 1 + 2)": "mixer", "libc / other": "vector indexing / libc / other",
             "predictor glue (predict/update/contexts)": "predictor glue", "byte match models": "byte match models",
             "discovery pool": "discovery pool (own code)", "APM / SSE": "APM ×3"}
    keys = [k for k in sorted(ca, key=lambda k: -ca[k]) if ca[k] >= 0.9]
    rest_a = sum(v for k, v in ca.items() if k not in keys); rest_b = sum(v for k, v in cb.items() if k not in keys)
    labels = [short.get(k, k) for k in keys] + ["everything else"]
    va = [ca[k] for k in keys] + [rest_a]; vb = [cb.get(k, 0) for k in keys] + [rest_b]
    fig, ax = plt.subplots(figsize=(7.2, 3.6))
    y = np.arange(len(labels))
    ax.barh(y - 0.2, va, 0.38, color=HP40, label="HP40")
    ax.barh(y + 0.2, vb, 0.38, color=LITE, label="HP40-Lite")
    for i, v in enumerate(va): ax.text(v + 0.5, i - 0.2, "%.1f" % v, va="center", fontsize=7.5, color=INK2)
    ax.set_yticks(y, labels); ax.invert_yaxis(); ax.grid(axis="y", visible=False)
    ax.set_xlabel("share of self CPU time (%), -fno-inline build, enwik8 1 MiB, --mem 18")
    ax.set_title("Where the CPU goes: one table load per context model per bit")
    ax.legend(loc="lower right")
    save(fig, os.path.join(FIG, "cpu_profile.png"))

if __name__ == "__main__":
    fig_cpu()
