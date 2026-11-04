#!/usr/bin/env python3
"""bench.py CODEC OUT.jsonl FILE... : compress, decompress, verify; one JSON line per file.
CODEC is a name from CODECS. Measures wall/user/sys/peak RSS for each direction."""
import hashlib, json, os, resource, subprocess, sys, tempfile, time

LAB = os.path.dirname(os.path.abspath(__file__))
BIN = os.environ.get("HP_BIN", os.path.join(LAB, "bin"))   # dir holding hp_v40, hp_v40lite, ...

def hp(binname, mem):
    b = os.path.join(BIN, binname)
    return ([b, "c", "--mem", str(mem), "{i}", "{o}"], [b, "d", "{i}", "{o}"])

def pipe(c, d):
    return (["sh", "-c", c + " < {i} > {o}"], ["sh", "-c", d + " < {i} > {o}"])

def zpaq(method):
    # zpaq is an archiver: the archive holds the file under its base name; its whole size is the output
    return (["sh", "-c", 'cd "$(dirname {i})" && rm -f {o}.zpaq && zpaq a {o}.zpaq "$(basename {i})" ' + method
             + ' -t1 > /dev/null && mv {o}.zpaq {o}'],
            ["sh", "-c", 'rm -rf {o}.d && ln -sf {i} {o}.zpaq && zpaq x {o}.zpaq -to {o}.d -t1 > /dev/null'
             ' && mv {o}.d/* {o} && rm -rf {o}.d {o}.zpaq'])

CODECS = {
    "hp40":      hp("hp_v40", 22),
    "hp40lite":  hp("hp_v40lite", 22),
    "gzip9":     pipe("gzip -9 -c", "gzip -d -c"),
    "bzip2-9":   pipe("bzip2 -9 -c", "bzip2 -d -c"),
    "xz9e":      pipe("xz -9e -c -T1", "xz -d -c"),
    "zstd19":    pipe("zstd -19 --long=27 -q -c --single-thread", "zstd -d --long=27 -q -c"),
    "zstd22u":   pipe("zstd --ultra -22 --long=27 -q -c --single-thread", "zstd -d --long=27 -q -c"),
    "brotli11":  pipe("brotli -q 11 -w 24 -c", "brotli -d -c"),
    "lz4hc":     pipe("lz4 -12 -q -c", "lz4 -d -q -c"),
    # other context-mixing compressors, binaries on PATH: paq8px v217, zpaq 7.15, lpaq1
    "paq8px8":   (["paq8px", "-8", "{i}", "{o}"], ["paq8px", "-d", "{i}", "{o}"]),
    "zpaq5":     zpaq("-m5"),
    "lpaq1_9":   (["lpaq1", "9", "{i}", "{o}"], ["lpaq1", "d", "{i}", "{o}"]),
}
for m in (16, 18, 20, 22, 24):
    CODECS[f"hp40_m{m}"] = hp("hp_v40", m)
    CODECS[f"hp40lite_m{m}"] = hp("hp_v40lite", m)
# extra binaries: hp:<binname>:<mem>
def codec(name):
    if name.startswith("hpd:"):          # hpd:<bin>:<space-separated compress args>
        _, b, args = name.split(":", 2)
        exe, a = os.path.join(BIN, b), args.split()
        dargs = ["-t", a[a.index("-t") + 1]] if "-t" in a else []   # decoder takes only -t
        return ([exe, "c"] + a + ["{i}", "{o}"], [exe, "d"] + dargs + ["{i}", "{o}"])
    if name.startswith("hp:"):
        _, b, m = name.split(":"); return hp(b, int(m))
    return CODECS[name]

def run(cmd, i, o):
    cmd = [c.replace("{i}", i).replace("{o}", o) for c in cmd]
    t0 = time.perf_counter()
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    err = p.stderr.read()
    _, status, ru = os.wait4(p.pid, 0)
    wall = time.perf_counter() - t0
    p.returncode = os.waitstatus_to_exitcode(status)
    # for "sh -c" pipes ru covers the shell's children too (wait4 reports the process tree it reaped)
    return dict(rc=p.returncode, wall=wall, user=ru.ru_utime, sys=ru.ru_stime, maxrss_kb=ru.ru_maxrss,
                err=err.decode(errors="replace")[-300:])

def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""): h.update(b)
    return h.hexdigest()

def main():
    name, out, files = sys.argv[1], sys.argv[2], sys.argv[3:]
    nodec = os.environ.get("NODEC") == "1"
    c, d = codec(name)
    tdir = os.environ.get("BENCH_TMP", os.path.join(LAB, "tmp")); os.makedirs(tdir, exist_ok=True)
    tmp = tempfile.mkdtemp(dir=tdir)
    for f in files:
        arc, back = os.path.join(tmp, "a"), os.path.join(tmp, "b")
        rc = run(c, f, arc)
        r = dict(codec=name, file=f, raw=os.path.getsize(f), comp=os.path.getsize(arc) if rc["rc"] == 0 else None, c=rc)
        if not nodec and rc["rc"] == 0:
            r["d"] = run(d, arc, back)
            r["rt_ok"] = r["d"]["rc"] == 0 and sha(back) == sha(f)
        r["bpc"] = 8 * r["comp"] / r["raw"] if r["comp"] else None
        with open(out, "a") as fo: fo.write(json.dumps(r) + "\n")
        print(name, os.path.basename(f), r["raw"], r["comp"], "%.3f" % (r["bpc"] or -1), "%.1fs" % rc["wall"], r.get("rt_ok"), flush=True)
        for p in (arc, back):
            if os.path.exists(p): os.remove(p)
    os.rmdir(tmp)

if __name__ == "__main__":
    main()
