"""Expert index -> name for hp v40 (order of Predictor::predict, bias excluded)."""
CM_V40 = ["o1", "o2", "o3", "o4", "o6", "word", "sp13", "sp24", "col", "tag", "wbi",
          "wstr", "brk", "link", "num", "sen", "sentst", "sentmem"]
MATCH_V40 = ["m3", "m4", "m6", "m10", "m16", "m8", "m1", "m2", "m5"]

def names(cms=CM_V40, matches=MATCH_V40, sutf8=True, nwm=4, hebb=True, disc=12):
    out = []
    for c in cms:
        out += [c + ".a", c + ".b"]          # ContextModel::kOutputs = 2 (state + run/PY)
    out += matches
    if sutf8: out.append("sutf8")
    out += ["wm%d" % (i + 1) for i in range(nwm)]
    if hebb: out.append("hebb")
    for i in range(disc):
        out += ["disc%d.a" % i, "disc%d.b" % i]
    return out

FAMILY = {}
for c in CM_V40: FAMILY[c] = "cm:" + c
def family(n):
    base = n.split(".")[0]
    if base.startswith("disc"): return "disc"
    if base.startswith("wm"): return "wordmatch"
    if base in MATCH_V40: return "match"
    return base

CM_LITE = ["o1", "o2", "o3", "o4", "o6", "word", "sp13", "sp24", "col", "tag", "brk", "sentst"]
MATCH_LITE = ["m3", "m4", "m6", "m10", "m16", "m1", "m2"]

def names_for(variant):
    if variant == "lite":
        return names(CM_LITE, MATCH_LITE, sutf8=True, nwm=3, hebb=False, disc=4)
    return names()
