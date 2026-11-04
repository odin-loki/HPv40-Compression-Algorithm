#!/usr/bin/env bash
# build_pdf.sh — build HP40_hpd_paper.pdf (Parts I and II as one paper).
# Needs python3, pandoc (3.x) and xelatex (TeX Live or MiKTeX) with the TeX Gyre
# and DejaVu fonts. Run from anywhere.
set -euo pipefail
cd "$(dirname "$0")/.."
PY=python3; command -v python3 >/dev/null 2>&1 && python3 -c "" 2>/dev/null || PY=python
"$PY" scripts/assemble.py
"$PY" scripts/combine.py
pandoc HP40_hpd_paper.md -o HP40_hpd_paper.pdf \
  --pdf-engine=xelatex -H latex/header.tex --lua-filter latex/tables.lua \
  --resource-path=. \
  -V papersize=a4 -V geometry:margin=2.2cm -V fontsize=10pt \
  -V colorlinks=true -V linkcolor=NavyBlue -V urlcolor=NavyBlue -V toccolor=NavyBlue
echo "wrote $(pwd)/HP40_hpd_paper.pdf"
