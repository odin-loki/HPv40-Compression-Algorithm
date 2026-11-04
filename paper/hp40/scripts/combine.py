#!/usr/bin/env python3
"""combine.py — merge Part I (HP40_paper.md) and Part II (HPD_part2.md) into one paper.

Writes HP40_hpd_paper.md next to them: one title and abstract, Part I as
sections 1-9, Part II as sections 10-20, then the appendices. Part II's section,
table and figure numbers are shifted to follow Part I's, and links to files in
the repository become links to the GitHub repository so the paper stands alone.
Both parts stay the sources; re-run this after editing either.
Build the PDF with scripts/build_pdf.sh.
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = "https://github.com/odin-loki/HPv40-Compression-Algorithm"
VERSION = "1.2.0"
DATE = "2026-11-05"
SEC_SHIFT = 9                                   # Part II section n -> n + 9
TABLES = {"1": "4", "3": "5", "4": "6"}         # Part II table numbers -> combined
FIGURES = {"1": "12", "2": "13"}                # Part II figure numbers -> combined


def read(name):
    return open(os.path.join(ROOT, name), encoding="utf-8").read()


def cut(s, start, end=None):
    a = s.index(start)
    return s[a:s.index(end, a)] if end else s[a:]


p1 = read("HP40_paper.md")
p2 = read("HPD_part2.md")

# ---------------------------------------------------------------- Part I
abstract1 = cut(p1, "## Abstract\n", "## Contents\n")
body1 = cut(p1, "## 1. Introduction\n", "## Appendix A.")
appendix = cut(p1, "## Appendix A.")

old = body1[body1.index("**Follow-up.** [Part II](HPD_part2.md)"):]
body1 = body1.replace(old, """**Follow-up.** Part II (§10–20) applies these findings: **hpd** is HP40-Lite
with bucketed, prefetched tables, demand-zero memory, a record model, an x86
transform and parallel blocks. It is 1.55% smaller than HP40 on the GP set,
5.6× faster and uses 1/18 of the memory, and 19.4% smaller than xz -9e on the
full Silesia corpus.

""")

# ---------------------------------------------------------------- Part II
intro2 = cut(p2, "Part I found that", "## 1. Method\n")
body2 = cut(p2, "## 1. Method\n")
part2 = intro2 + body2

part2 = part2.replace("`compress/hpd/GPU_PLAN.md` §9", "`compress/hpd/GPU_PLAN.md` @@PLAN9@@")
part2 = re.sub(r"^## (\d+)\. ", lambda m: "## %d. " % (int(m.group(1)) + SEC_SHIFT), part2, flags=re.M)
part2 = re.sub(r"§§(\d+)–(\d+)", lambda m: "§§%d–%d" % (int(m.group(1)) + SEC_SHIFT, int(m.group(2)) + SEC_SHIFT), part2)
part2 = re.sub(r"(?<!§)§(\d+)(?!\d)", lambda m: "§%d" % (int(m.group(1)) + SEC_SHIFT), part2)
part2 = part2.replace("@@PLAN9@@", "§9")
part2 = re.sub(r"Table (\d+)\b", lambda m: "Table @@%s@@" % TABLES.get(m.group(1), m.group(1)), part2)
part2 = re.sub(r"Figure (\d+)\b", lambda m: "Figure @@%s@@" % FIGURES.get(m.group(1), m.group(1)), part2)
part2 = re.sub(r"@@(\d+)@@", r"\1", part2)
part2 = part2.replace("](../../dist/hpd-%s-src.zip)" % VERSION, "](%s/blob/main/dist/hpd-%s-src.zip)" % (REPO, VERSION))
part2 = part2.replace("](../../compress/hpd/)", "](%s/tree/main/compress/hpd)" % REPO)
assert "../../" not in part2 and "HP40_paper.md" not in part2

# ---------------------------------------------------------------- front matter
abstract = abstract1.replace("**Headline numbers (GP set, 27,033,797 bytes):**", """Part II applies these findings. **hpd** is HP40-Lite rebuilt around the
profile: cache-line buckets with checksums, demand-zero tables, a record
model, an x86 transform, pass-through of incompressible data and parallel
blocks. It is **1.55% smaller than HP40, 5.6× faster and uses 1/18 of its
memory**, writing **1.823 bits/byte on the GP set (21.3% below xz -9e)** and
1.474 bits/byte on the full 212 MB Silesia corpus (19.4% below xz -9e), the
smallest archive of every tested compressor on 40 of 41 GP files and all 12
Silesia files. A GPU build writes byte-identical archives, but on an RTX 3090
it only reaches 0.84–0.95× the speed of a 64-thread CPU, and the small shards
it needs cost 27–45% in ratio.

**Headline numbers for HP40 and HP40-Lite (GP set, 27,033,797 bytes):**""")


def toc(text):
    return " · ".join(m.group(1) for m in re.finditer(r"^## (\d+\. [^\n]+)$", text, re.M))


front = f"""---
title: "HP40 and hpd: Profiling, Data-Mining and Rebuilding an Integer-Exact Context Mixer for General-Purpose Compression"
author: "Odin Loch"
date: "{DATE} · hpd {VERSION}"
---

**Code, data and scripts:** <{REPO}> ·
**Source package:** `hpd-{VERSION}-src.zip` (§18) ·
**Status:** Research report in two parts. All numbers are measured in this
study unless cited to the upstream lab log.

{abstract.rstrip()}

## Contents

**Part I — HP40 and HP40-Lite as general-purpose compressors.**
{toc(body1)}

**Part II — hpd: turning HP40 into a day-to-day compressor.**
{toc(part2)}

**Appendices.** A. Reproducing Part I · B. Per-file archive bytes

"""

out = (front
       + "# Part I — HP40 and HP40-Lite as General-Purpose Compressors\n\n" + body1.rstrip() + "\n\n"
       + "# Part II — hpd: Turning HP40 into a Day-to-Day Compressor\n\n" + part2.rstrip() + "\n\n"
       + "# Appendices\n\n" + appendix.rstrip() + "\n")
out = out.replace("## Appendix A. Reproducing this paper", "## Appendix A. Reproducing Part I")
# One caption per figure: the italic line under each image (the full caption,
# which GitHub shows) replaces the image's shorter alt text.
out, nfig = re.subn(r"!\[[^\]]*\]\(([^)]+)\)\n\n\*(Figure \d+\.)([^\n]*?)\*\n",
                    lambda m: "![**%s**%s](%s)\n" % (m.group(2), m.group(3), m.group(1)), out)
assert nfig == 13, nfig
open(os.path.join(ROOT, "HP40_hpd_paper.md"), "w", encoding="utf-8", newline="\n").write(out)
print("wrote HP40_hpd_paper.md", out.count("\n"), "lines")
