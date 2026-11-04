#!/usr/bin/env bash
# roundtrip.sh HPD [FILE...] : compress + decompress + compare, including edge cases.
set -euo pipefail
HPD="$1"; shift || true
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
: > "$T/empty"; printf 'a' > "$T/one"; head -c 100000 /dev/urandom > "$T/random"
python3 -c "import sys; sys.stdout.write('hello hpd\\n' * 30000)" > "$T/repeat"
fail=0
for f in "$T/empty" "$T/one" "$T/random" "$T/repeat" "$@"; do
  for t in 1 3; do
    "$HPD" c -t $t "$f" "$T/a" 2>/dev/null && "$HPD" d -t $t "$T/a" "$T/b" 2>/dev/null
    if cmp -s "$f" "$T/b"; then echo "ok   t=$t $(basename "$f") $(stat -c%s "$f") -> $(stat -c%s "$T/a")"; else echo "FAIL t=$t $f"; fail=1; fi
  done
done
exit $fail
