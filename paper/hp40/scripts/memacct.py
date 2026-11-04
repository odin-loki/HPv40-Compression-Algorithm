#!/usr/bin/env python3
"""Analytic table-memory accounting for HP40 / HP40-Lite as a function of --mem.
Mirrors Predictor's constructor (CompressionAlgorithm 0b1ced2 + v40 flags):
ContextModel = 2 B/slot; MatchModel / WordMatchModel = own 2^buf history buffer + 4 B x 2^tab index."""
SLOT_MAX = 32
LITE_DROP = {"wbi", "wstr", "link", "num", "sen", "sentmem", "m8", "m5", "wm4"}
LITE_DISC = 4

def slot_bits(base, delta):
    if delta < 0: delta = 0
    b = base + delta
    if delta >= 2: b += 1          # HP_SLOT_GROW_EXTRA
    if delta == 1: b += 8          # HP_SLOT_WORD2..WORD9
    return max(16, min(SLOT_MAX, b))

def add_bits(b, e): return max(16, min(SLOT_MAX, b + e))

def components(mem, variant="hp40"):
    t = mem; buf = min(28, t + 4); mb = max(16, min(28, t + 1))   # buf_bits, MATCH_GROW index
    drop = LITE_DROP if variant == "lite" else set()
    cm = {"o1": add_bits(slot_bits(t, -2), 0), "o2": add_bits(slot_bits(t, -2), 0),
          "o3": add_bits(slot_bits(t, 0), 5), "o4": add_bits(slot_bits(t, 0), 5),
          "o6": add_bits(slot_bits(t, 0), 6), "word": slot_bits(t, 1),
          "col": slot_bits(t, 0), "tag": slot_bits(t, 2), "wbi": slot_bits(t, 1),
          "sp13": slot_bits(t, 0), "sp24": slot_bits(t, 0), "wstr": slot_bits(t, 2),
          "brk": slot_bits(t, 0), "link": slot_bits(t, 2), "num": slot_bits(t, 0),
          "sen": slot_bits(t, 2), "sentst": slot_bits(t, 3), "sentmem": slot_bits(t, 0)}
    out = {}
    for k, b in cm.items():
        if k == "wbi" and "wbi" in drop: b = 16          # switched off, kept at 2^16 slots
        elif k in drop: continue                         # compiled out
        out["CM " + k] = 2 * (1 << b)
    mm = (1 << buf) + 4 * (1 << mb)
    nmatch = 9 - len({"m8", "m5"} & drop)
    out["byte match x%d" % nmatch] = nmatch * mm
    out["sparse-UTF-8 match"] = mm
    nwm = 3 if "wm4" in drop else 4
    out["word match x%d" % nwm] = nwm * mm
    nd = LITE_DISC if variant == "lite" else 12
    out["discovery pool x%d" % nd] = nd * 2 * (1 << t)
    out["Hebbian"] = 2 * (1 << t) + 16 * (1 << t)       # states + 16-B synapses (built in both)
    return out

def total(mem, variant="hp40"):
    return sum(components(mem, variant).values())

if __name__ == "__main__":
    for v in ("hp40", "lite"):
        for m in (16, 18, 20, 22):
            c = components(m, v)
            top = sorted(c.items(), key=lambda kv: -kv[1])[:4]
            print(v, m, "%.0f MiB" % (total(m, v) / 2**20), [(k, "%.0f" % (x / 2**20)) for k, x in top])
