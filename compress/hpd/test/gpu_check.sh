#!/usr/bin/env bash
# gpu_check.sh HPD HPD_GPU [FILE...] : the GPU build must write the same archive
# as the CPU build with the same settings, and each must decode the other's.
set -euo pipefail
HPD="$1"; HPDG="$2"; shift 2
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
: > "$T/empty"; printf 'a' > "$T/one"; head -c 50000 /dev/urandom > "$T/random"
python3 -c "import sys; sys.stdout.write('hello hpd gpu\n' * 20000)" > "$T/repeat"
fail=0
for f in "$T/empty" "$T/one" "$T/random" "$T/repeat" "$@"; do
  for opt in "-B 16" "-B 64 -x 1"; do
    "$HPDG" c -q -f -G $opt "$f" "$T/g"
    "$HPD"  c -q -f -M 18 $opt "$f" "$T/c"
    "$HPD"  d -q -f "$T/g" "$T/gd"
    "$HPDG" d -q -f -G "$T/c" "$T/cd"
    if cmp -s "$T/g" "$T/c" && cmp -s "$T/gd" "$f" && cmp -s "$T/cd" "$f"; then
      echo "ok   [$opt] $(basename "$f") $(stat -c%s "$f") -> $(stat -c%s "$T/g") (GPU == CPU archive, cross-decoded)"
    else echo "FAIL [$opt] $f"; fail=1; fi
  done
done
exit $fail
