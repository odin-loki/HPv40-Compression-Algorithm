#!/usr/bin/env bash
# pc_shard_bench.sh FILE [THREADS] — run on OdinsPC under WSL2 (or any Linux); any working directory.
# Reports CPU / RAM / NVIDIA GPU, builds hpd, compresses FILE as auto, 16 MiB,
# 4 MiB, 1 MiB and 256 KiB shards, verifies every round trip, and writes
# compress/hpd/tools/pc_results.txt — paste that file back into the Claude session.
set -euo pipefail
FILE="$1"; THREADS="${2:-$(nproc)}"
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/.." && pwd)"
OUT="$HERE/pc_results.txt"
{
  echo "hpd shard benchmark  $(date -Is)"
  echo "CPU: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')  threads: $(nproc)"
  echo "RAM: $(free -g | awk '/Mem:/{print $2}') GiB"
  if command -v nvidia-smi >/dev/null; then
    nvidia-smi --query-gpu=name,memory.total,compute_cap,driver_version --format=csv
  else echo "GPU: nvidia-smi not found"; fi
  if command -v nvcc >/dev/null; then nvcc --version | tail -2; else echo "CUDA toolkit: nvcc not found"; fi
} > "$OUT"
make -s -C "$ROOT" >> "$OUT" 2>&1
HPD="$ROOT/build/hpd"
SIZE=$(stat -c%s "$FILE")
echo "input $FILE  $SIZE bytes  threads $THREADS" >> "$OUT"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
for kib in 0 16384 4096 1024 256; do
  args=(c -t "$THREADS"); [ "$kib" -gt 0 ] && args+=(-B "$kib")
  s=$(date +%s.%N); "$HPD" "${args[@]}" "$FILE" "$T/a" 2>/dev/null; e=$(date +%s.%N)
  "$HPD" d -t "$THREADS" "$T/a" "$T/b" 2>/dev/null; f=$(date +%s.%N)
  ok=$(cmp -s "$FILE" "$T/b" && echo ok || echo FAIL)
  c=$(stat -c%s "$T/a")
  shards=$([ "$kib" -gt 0 ] && echo $(( (SIZE + kib * 1024 - 1) / (kib * 1024) )) || echo auto)
  awk -v l="$([ "$kib" -gt 0 ] && echo "$kib KiB" || echo auto)" -v sh="$shards" -v c="$c" -v n="$SIZE" -v s="$s" -v e="$e" -v f="$f" -v ok="$ok" \
    'BEGIN{printf "%-10s shards %6s  bytes %12d  bpc %.3f  compress %7.1f s (%8.0f KiB/s)  decompress %7.1f s  roundtrip %s\n", l, sh, c, 8*c/n, e-s, n/1024/(e-s), f-e, ok}' >> "$OUT"
done
cat "$OUT"
