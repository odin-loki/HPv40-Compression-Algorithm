#!/usr/bin/env python3
"""assemble.py — concatenate sections/ into HP40_paper.md (section files are the source)."""
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ORDER = ["00_abstract", "01_intro", "02_architecture", "03_method", "04_results", "06_profile",
         "08_mining", "07_lite", "09_discussion", "10_conclusion"]
parts = [open(os.path.join(ROOT, "sections", n + ".md")).read().strip() + "\n" for n in ORDER]
tables = open(os.path.join(ROOT, "data", "tables.md")).read()
perfile = tables[tables.index("### Table C"):]
perfile = perfile.replace("### Table C — per file, archive bytes", "## Appendix B. Per-file archive bytes\n\nBold marks the smallest archive in the row. `pic` and `ptt5` are the same file.")
parts.append(perfile)
open(os.path.join(ROOT, "HP40_paper.md"), "w").write("\n".join(parts))
print("wrote HP40_paper.md", sum(p.count("\n") for p in parts), "lines")
