#!/usr/bin/env bash
# fuzz.sh HPD_BIN [N_ITER] : mutation fuzzer for `hpd d` (any archive version).
#
# Builds valid archives from a few small seed inputs with HPD_BIN itself, mutates
# them (bit flips, byte overwrites, truncation, appended garbage, splices, pure
# random files, and -- for v2 archives -- edits of every header field and block
# count / block duplication), decodes each mutant under `timeout` and a memory
# limit, and classifies the outcome:
#   accepted-identical   exit 0, output == original input (mutation was harmless)
#   accepted-DIFFERENT   exit 0, output != original      (silent corruption; v2 has no checksum)
#   clean-error          exit 1/2 with a message, no sanitizer report
#   mem-limit            bad_alloc / ASan allocation-size-too-big / rss limit
#   timeout              killed by `timeout` (declared size known to be inflated)
#   HANG?                timeout although the declared size is not inflated (v2 only)
#   SANITIZER            ASan/UBSan/TSan/LSan report
#   CRASH-<sig>          killed by a signal (SEGV, FPE, ABRT without bad_alloc, ...)
#
# Environment knobs:
#   JOBS=8          parallel decoders            TIMEOUT=auto   seconds per decode
#   MEM_MB=3072     memory limit per decoder     SEED=1         RNG seed (reproducible)
#   OUT=dir         work/results directory (default ./fuzz_out.<pid>)
#   KEEP=25         samples kept per interesting class
#   INPUTS=dir      extra seed inputs (small files) besides the built-in ones
#   DTHREADS="1 3"  -t values used by the decoder (alternated)
#
# Re-run on any build:  bash test/fuzz.sh build/hpd 2000
# ASan build:           g++ -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
#                           -std=c++20 -pthread -I include src/hpd.cpp -o build/hpd_asan
#                       bash test/fuzz.sh build/hpd_asan 5000
# A saved sample is replayed with the command in its repro.sh.
set -u
HPD=$(readlink -f "${1:?usage: fuzz.sh HPD_BIN [N_ITER]}")
N=${2:-1000}
JOBS=${JOBS:-8}; MEM_MB=${MEM_MB:-3072}; SEED=${SEED:-1}; KEEP=${KEEP:-25}
DTHREADS=${DTHREADS:-"1 3"}
OUT=${OUT:-$PWD/fuzz_out.$$}
mkdir -p "$OUT"/{seeds,cases,findings,tmp}
OUT=$(readlink -f "$OUT")
command -v python3 >/dev/null || { echo "python3 required"; exit 2; }

SAN=none
if nm "$HPD" 2>/dev/null | grep -q '__asan_init' || ldd "$HPD" 2>/dev/null | grep -q libasan; then SAN=asan
elif nm "$HPD" 2>/dev/null | grep -q '__tsan_init' || ldd "$HPD" 2>/dev/null | grep -q libtsan; then SAN=tsan
elif nm "$HPD" 2>/dev/null | grep -q '__ubsan_handle' || ldd "$HPD" 2>/dev/null | grep -q libubsan; then SAN=ubsan; fi
echo "binary: $HPD  sanitizer: $SAN  iterations: $N  jobs: $JOBS  mem: ${MEM_MB} MiB  out: $OUT"

# ---------------------------------------------------------------- seeds
python3 - "$OUT/seeds" "${INPUTS:-}" <<'PYEOF'
import os, random, struct, sys
d, extra = sys.argv[1], sys.argv[2]
r = random.Random(12345)
words = "the of and to in is was for on that with as by at from his her it an were are which this be or has had first one their its new after who they two all also".split()
def text(n): 
    s = []
    while sum(len(w) + 1 for w in s) < n: s.append(r.choice(words))
    return (' '.join(s) + '\n').encode()[:n]
def code(n):
    out = bytearray(); tg = [r.randrange(1 << 16) for _ in range(12)]
    while len(out) < n:
        out += bytes(r.getrandbits(7) for _ in range(r.randrange(0, 6)))
        out += b'\xe8' + struct.pack('<I', (r.choice(tg) - len(out) - 5) & 0xffffffff)
    return bytes(out[:n])
seeds = [  # name, data, compressor options
    ('text6k',    text(6000), '-t 1'),
    ('records4k', b''.join(struct.pack('<IHH8s', i, i * 7 % 1000, i % 13, b'rec%05d' % i) for i in range(256)), '-t 1'),
    ('random2k',  bytes(r.getrandbits(8) for _ in range(2048)), '-t 1'),
    ('code6k_x86', code(6000), '-t 1 -x 1'),
    ('zeros12k_mb', bytes(12000), '-t 2 -B 4'),
    ('text12k_mb', text(12000), '-t 3 -B 4'),
    ('one',       b'a', '-t 1'),
    ('empty',     b'', '-t 1'),
    ('text3k_m20', text(3000), '-t 1 -m 20 -9'),
]
if extra and os.path.isdir(extra):
    for f in sorted(os.listdir(extra))[:8]:
        p = os.path.join(extra, f)
        if os.path.isfile(p) and os.path.getsize(p) <= 65536:
            seeds.append(('x_' + f, open(p, 'rb').read(), '-t 1'))
with open(os.path.join(d, 'list.tsv'), 'w') as L:
    for i, (name, data, opt) in enumerate(seeds):
        open(os.path.join(d, '%d.in' % i), 'wb').write(data)
        L.write('%d\t%s\t%s\n' % (i, name, opt))
PYEOF

echo "compressing seeds..."
maxbase=0
while IFS=$'\t' read -r i name opt; do
  "$HPD" c $opt "$OUT/seeds/$i.in" "$OUT/seeds/$i.hpd" 2> "$OUT/seeds/$i.c.err" || { echo "seed $name: compress failed"; cat "$OUT/seeds/$i.c.err"; exit 1; }
  t0=$(date +%s%N)
  "$HPD" d -t 1 "$OUT/seeds/$i.hpd" "$OUT/seeds/$i.out" 2> "$OUT/seeds/$i.d.err" || { echo "seed $name: decode failed"; exit 1; }
  t=$(( ($(date +%s%N) - t0) / 1000000 ))
  cmp -s "$OUT/seeds/$i.in" "$OUT/seeds/$i.out" || { echo "seed $name: round trip FAILED"; exit 1; }
  [ $t -gt $maxbase ] && maxbase=$t
  printf '  seed %-12s %6d B -> %6d B  (%s)  decode %d ms\n' "$name" "$(stat -c%s "$OUT/seeds/$i.in")" "$(stat -c%s "$OUT/seeds/$i.hpd")" "$opt" $t
done < "$OUT/seeds/list.tsv"
if [ -z "${TIMEOUT:-}" ] || [ "$TIMEOUT" = auto ]; then
  TIMEOUT=$(( maxbase * 5 / 1000 + 8 )); fi
echo "timeout per decode: ${TIMEOUT}s (slowest seed decode ${maxbase} ms)"

# ---------------------------------------------------------------- mutants
python3 - "$OUT" "$N" "$SEED" <<'PYEOF'
import os, random, sys
out, N, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
r = random.Random(seed)
seeds = []
for line in open(os.path.join(out, 'seeds', 'list.tsv')):
    i, name, opt = line.rstrip('\n').split('\t')
    seeds.append((int(i), name, open(os.path.join(out, 'seeds', '%s.hpd' % i), 'rb').read(),
                  len(open(os.path.join(out, 'seeds', '%s.in' % i), 'rb').read())))

def u(b, o, n): return int.from_bytes(b[o:o + n], 'little')
def setu(b, o, n, v): b[o:o + n] = (v % (1 << (8 * n))).to_bytes(n, 'little')

def parse_v2(a):
    if len(a) < 17 or a[:4] != b'HPD1' or a[4] != 2: return None
    L = {'n': (5, 8), 'nb': (13, 4), 'blocks': []}
    p, nb = 17, u(a, 13, 4)
    for k in range(min(nb, 4096)):
        if p + 13 > len(a): break
        L['blocks'].append(p); p += 13 + u(a, p + 9, 4)
    return L

U8 = [0, 1, 2, 3, 4, 11, 12, 13, 15, 16, 17, 20, 22, 27, 28, 29, 31, 32, 33, 63, 64, 127, 128, 254, 255]
def U32(orig): return [0, 1, 2, orig - 1, orig + 1, orig * 2, orig + 4096, 1 << 16, 1 << 20, 1 << 24, 0x7fffffff, 0x80000000, 0xfffffffe, 0xffffffff]
def U64(orig): return [0, 1, orig - 1, orig + 1, orig * 2, 1 << 32, (1 << 32) - 1, 1 << 40, 1 << 63, (1 << 64) - 1]

def m_bitflip(a, s):
    if not a: return a, 'noop'
    k = r.choice([1, 1, 1, 2, 3, 8]); offs = []
    for _ in range(k):
        o = r.randrange(len(a)); a[o] ^= 1 << r.randrange(8); offs.append(o)
    return a, 'bitflip@' + ','.join(map(str, offs))
def m_bitflip_payload_tail(a, s):   # flips late in the stream (affects only the end of decoding)
    if len(a) < 40: return m_bitflip(a, s)
    o = r.randrange(len(a) * 3 // 4, len(a)); a[o] ^= 1 << r.randrange(8)
    return a, 'tailflip@%d' % o
def m_byte(a, s):
    if not a: return a, 'noop'
    offs = []
    for _ in range(r.choice([1, 1, 2, 4])):
        o = r.randrange(len(a)); a[o] = r.choice([0, 0xff, 0x7f, 0x80, r.getrandbits(8)]); offs.append(o)
    return a, 'byte@' + ','.join(map(str, offs))
def m_trunc(a, s):
    n = r.randrange(len(a) + 1) if r.random() < 0.7 else max(0, len(a) - r.randrange(1, 6))
    return a[:n], 'trunc->%d' % n
def m_append(a, s):
    g = bytes(r.getrandbits(8) for _ in range(r.choice([1, 4, 13, 64, 300])))
    return a + g, 'append+%d' % len(g)
def m_splice(a, s):
    other = r.choice(seeds); b = other[2]
    i = r.randrange(len(a) + 1); j = r.randrange(len(b) + 1)
    return a[:i] + b[j:], 'splice %d+%s[%d:]' % (i, other[1], j)
def m_random(a, s):
    n = r.choice([0, 1, 5, 16, 17, 29, 30, 31, 100, 1000, 4000])
    g = bytearray(r.getrandbits(8) for _ in range(n))
    if r.random() < 0.6 and len(a) >= 5: g[:5] = a[:5]            # keep magic + version
    return g, 'random%d' % n
def m_generic_field(a, s):     # format-agnostic "interesting value" at a small offset
    if len(a) < 2: return m_append(a, s)
    w = r.choice([1, 1, 2, 4, 4, 8])
    o = r.randrange(0, max(1, min(len(a) - w + 1, 64)))
    if o + w > len(a): return m_byte(a, s)
    orig = u(a, o, w)
    v = r.choice(U8 if w == 1 else (U32(orig) if w <= 4 else U64(orig)))
    setu(a, o, w, v)
    return a, 'field%d@%d=%d' % (w, o, v % (1 << (8 * w)))
def m_v2_field(a, s):
    L = parse_v2(a)
    if not L or not L['blocks']: return m_generic_field(a, s)
    which = r.random()
    if which < 0.12:
        o, w = L['n']; orig = u(a, o, w); v = r.choice(U64(orig)); setu(a, o, w, v); return a, 'v2.n=%d' % (v % (1 << 64))
    if which < 0.22:
        o, w = L['nb']; orig = u(a, o, w); v = r.choice([orig + 1, orig * 2, 0, 0xffffffff, orig - 1]); setu(a, o, w, v); return a, 'v2.nb=%d' % (v % (1 << 32))
    p = r.choice(L['blocks'])
    f = r.choice(['method', 'filters', 'table_bits', 'slot_extra', 'disc_slots', 'raw', 'len', 'raw', 'len', 'table_bits+slot_extra'])
    if f == 'table_bits+slot_extra':
        a[p + 2] = r.choice([22, 24, 26, 28]); a[p + 3] = r.choice([8, 16, 32])
        return a, 'v2.blk@%d tb=%d se=%d' % (p, a[p + 2], a[p + 3])
    off = {'method': 0, 'filters': 1, 'table_bits': 2, 'slot_extra': 3, 'disc_slots': 4, 'raw': 5, 'len': 9}[f]
    if f in ('raw', 'len'):
        orig = u(a, p + off, 4); v = r.choice(U32(orig)) % (1 << 32); setu(a, p + off, 4, v)
        if f == 'raw' and r.random() < 0.5:   # keep the global size consistent so the per-block check passes
            setu(a, 5, 8, u(a, 5, 8) - orig + v)
        return a, 'v2.blk@%d.%s=%d' % (p, f, v)
    a[p + off] = r.choice(U8)
    return a, 'v2.blk@%d.%s=%d' % (p, f, a[p + off])
def m_v2_dup_block(a, s):
    L = parse_v2(a)
    if not L or not L['blocks']: return m_generic_field(a, s)
    bl = L['blocks']; k = r.randrange(len(bl)); p = bl[k]
    blk = a[p:p + 13 + u(a, p + 9, 4)]
    a = a[:p] + blk + a[p:]
    setu(a, 13, 4, u(a, 13, 4) + 1); setu(a, 5, 8, u(a, 5, 8) + u(blk, 5, 4))
    return a, 'v2.dup_block%d' % k
def m_v2_swap_blocks(a, s):
    L = parse_v2(a)
    if not L or len(L['blocks']) < 2: return m_v2_dup_block(a, s)
    bl = L['blocks'] + [len(a)]; k = r.randrange(len(bl) - 2)
    x = a[bl[k]:bl[k + 1]]; y = a[bl[k + 1]:bl[k + 2]]
    return a[:bl[k]] + y + x + a[bl[k + 2]:], 'v2.swap%d' % k

OPS = [(m_bitflip, 20), (m_bitflip_payload_tail, 6), (m_byte, 10), (m_trunc, 10), (m_append, 5), (m_splice, 4),
       (m_random, 5), (m_generic_field, 12), (m_v2_field, 20), (m_v2_dup_block, 3), (m_v2_swap_blocks, 3)]
tot = sum(w for _, w in OPS)
def pick():
    x = r.randrange(tot)
    for f, w in OPS:
        if x < w: return f
        x -= w
with open(os.path.join(out, 'cases', 'manifest.tsv'), 'w') as M:
    for cid in range(N):
        si, name, arch, rawlen = r.choice(seeds)
        a = bytearray(arch); descs = []; ops = []
        for _ in range(1 if r.random() < 0.75 else r.choice([2, 3])):
            f = pick(); a, d = f(a, si); a = bytearray(a); descs.append(d); ops.append(f.__name__[2:])
        open(os.path.join(out, 'cases', '%06d.hpd' % cid), 'wb').write(a)
        L = parse_v2(a)
        declared = u(a, 5, 8) if L else -1
        inflated = 1 if (L is None or declared > 4 * max(rawlen, 1024)) else 0
        M.write('%06d\t%d\t%s\t%s\t%s\t%d\n' % (cid, si, name, '+'.join(ops), ' ; '.join(descs), inflated))
PYEOF
echo "generated $N mutants"

# ---------------------------------------------------------------- run
run_case() {
  local cid=$1 si=$2 name=$3 op=$4 desc=$5 infl=$6
  local w; w=$(mktemp -d "$OUT/tmp/c.XXXXXX")
  local ts=($DTHREADS); local t=${ts[$((10#$cid % ${#ts[@]}))]}
  local cmd="\"$HPD\" d -t $t \"$OUT/cases/$cid.hpd\" \"$w/out\""
  local t0; t0=$(date +%s%N)
  if [ "$SAN" = none ] || [ "$SAN" = ubsan ]; then
    timeout -k 3 "$TIMEOUT" bash -c "ulimit -v $((MEM_MB * 1024)); exec $cmd" > /dev/null 2> "$w/err"
  else
    ASAN_OPTIONS="hard_rss_limit_mb=$MEM_MB:max_allocation_size_mb=$MEM_MB:allocator_may_return_null=0:detect_leaks=1:halt_on_error=1:handle_abort=1:${ASAN_OPTIONS:-}" \
    UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" TSAN_OPTIONS="halt_on_error=1:hard_rss_limit_mb=$MEM_MB" \
      timeout -k 3 "$TIMEOUT" bash -c "exec $cmd" > /dev/null 2> "$w/err"
  fi
  local rc=$?
  local ms=$(( ($(date +%s%N) - t0) / 1000000 ))
  local cls
  if grep -qE 'ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:|WARNING: ThreadSanitizer|ERROR: ThreadSanitizer' "$w/err"; then
    if grep -qE 'allocation-size-too-big|requested allocation size|rss-limit-exceeded|hard rss limit|out of memory|allocator is out of memory|mmap.*ENOMEM|failed to allocate' "$w/err"; then cls=mem-limit; else cls=SANITIZER; fi
  elif [ $rc = 124 ] || [ $rc = 137 ]; then
    if [ "$infl" = 1 ]; then cls=timeout; else cls='HANG?'; fi
  elif grep -qiE 'bad_alloc|cannot allocate|out of memory|length_error' "$w/err"; then cls=mem-limit
  elif [ $rc -ge 128 ]; then cls="CRASH-$(kill -l $((rc - 128)) 2>/dev/null || echo $rc)"
  elif [ $rc = 0 ]; then
    if cmp -s "$w/out" "$OUT/seeds/$si.in"; then cls=accepted-identical; else cls=accepted-DIFFERENT; fi
  else cls=clean-error; fi
  local msg; msg=$(grep -vE '^\s*$' "$w/err" | grep -vE '^(=+|\s+#)' | tail -1 | tr '\t' ' ' | sed "s#$OUT/cases/[0-9]*\.hpd#ARCHIVE#g" | cut -c1-90)
  printf '%s\t%s\t%s\t%s\t%d\t%d\t%s\t%s\n' "$cid" "$name" "$op" "$cls" "$rc" "$ms" "$msg" "$desc" >> "$OUT/results.raw"
  case $cls in clean-error|accepted-identical) ;; *)
    local dir="$OUT/findings/$cls"; mkdir -p "$dir"
    if [ "$(ls "$dir" | wc -l)" -lt "$KEEP" ]; then
      mkdir -p "$dir/$cid"; cp "$OUT/cases/$cid.hpd" "$w/err" "$dir/$cid/"
      printf '%s\n' "# $name | $op | $desc | rc=$rc" "$cmd" > "$dir/$cid/repro.sh"
    fi;;
  esac
  rm -rf "$w"
}
run_line() { local a b c d e f; IFS=$'\t' read -r a b c d e f <<< "$1"; run_case "$a" "$b" "$c" "$d" "$e" "$f"; }
export -f run_case run_line; export HPD OUT TIMEOUT MEM_MB SAN KEEP DTHREADS
t0=$(date +%s)
: > "$OUT/results.raw"
tr -d '\r' < "$OUT/cases/manifest.tsv" | xargs -P "$JOBS" -d '\n' -n 1 bash -c 'run_line "$1"' _
sort "$OUT/results.raw" > "$OUT/results.tsv"
echo "ran $N cases in $(( $(date +%s) - t0 )) s"

# ---------------------------------------------------------------- summary
python3 - "$OUT" "$HPD" "$SAN" <<'PYEOF' | tee "$OUT/summary.txt"
import collections, sys
out, hpd, san = sys.argv[1:4]
rows = [l.rstrip('\n').split('\t') for l in open(out + '/results.tsv') if l.strip()]
cls = collections.Counter(r[3] for r in rows)
print('binary %s (sanitizer: %s), %d cases' % (hpd, san, len(rows)))
for k, v in sorted(cls.items(), key=lambda x: -x[1]): print('  %-20s %6d  %5.1f%%' % (k, v, 100.0 * v / len(rows)))
ops = sorted(set(o for r in rows for o in r[2].split('+')))
keys = sorted(cls)
print('\nby mutation operator (case counted once per operator it used):')
print('  %-18s' % 'op' + ''.join('%14s' % k[:13] for k in keys))
for o in ops:
    c = collections.Counter(r[3] for r in rows if o in r[2].split('+'))
    print('  %-18s' % o + ''.join('%14d' % c.get(k, 0) for k in keys))
msgs = collections.Counter(r[6] for r in rows if r[3] == 'clean-error')
print('\nclean-error messages:')
for m, v in msgs.most_common(12): print('  %6d  %s' % (v, m))
bad = [r for r in rows if r[3] in ('SANITIZER', 'HANG?') or r[3].startswith('CRASH')]
print('\ninteresting (first 20):' if bad else '\nno sanitizer reports, crashes or suspected hangs')
for r in bad[:20]: print('  %s %-14s %s  %s | %s' % (r[0], r[3], r[1], r[7], r[6]))
print('\nsamples: %s/findings/<class>/<case>/repro.sh' % out)
PYEOF
