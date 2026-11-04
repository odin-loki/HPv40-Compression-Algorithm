#!/usr/bin/env bash
# build.sh OUTDIR — rebuild HP40, HP40-Lite (and the L5 variant) and the mining driver.
# Needs git, g++ >= 11 (C++20). The archives these binaries write are a separate
# format family per build: decode with the same binary that encoded.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="${1:-$ROOT/build}"
mkdir -p "$OUT"
SRC="$OUT/CompressionAlgorithm"
if [ ! -d "$SRC/.git" ]; then
  git clone https://github.com/odin-loki/CompressionAlgorithm "$SRC"
fi
git -C "$SRC" checkout -q 0b1ced2          # 2026-09-12, v47 board: every v40 switch still controllable
git -C "$SRC" checkout -q -- hp/include/hp/predictor.hpp
patch -d "$SRC" -p1 < "$ROOT/patches/hp40_lite_switches.patch"   # five default-on expert switches

V40=$(tr '\n' ' ' < "$ROOT/data/v40_flags.txt")
LITE=$(cat "$ROOT/data/lite_flags.txt")
L5=$(cat "$ROOT/data/lite_L5.txt")
CXX="${CXX:-g++}"
OPT="${OPT:--O3 -march=native}"
cd "$SRC/hp"
$CXX $OPT -std=c++20 -w $V40 -I include src/main.cpp -o "$OUT/hp_v40"
$CXX $OPT -std=c++20 -w $V40 $LITE -I include src/main.cpp -o "$OUT/hp_v40lite"
$CXX $OPT -std=c++20 -w $V40 $L5 -I include src/main.cpp -o "$OUT/hp_v40lite5"
$CXX -O2 -march=native -std=c++20 -w $V40 -I include "$HERE/mine.cpp" -o "$OUT/mine_v40"
$CXX -O2 -march=native -std=c++20 -w $V40 $LITE -I include "$HERE/mine.cpp" -o "$OUT/mine_v40lite"
echo "built: $(ls "$OUT" | grep -v CompressionAlgorithm | tr '\n' ' ')"
echo "anchor check: $OUT/hp_v40 c --mem 22 enwik8.8mb out.hp  ->  1,721,462 bytes"
