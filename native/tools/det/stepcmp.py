#!/usr/bin/env python3
"""Compare two NP_DET_TRACE files (src/Game/NpDet.cpp) step by step.
   tools/det/stepcmp.py a.trace b.trace [more.trace ...]    first file is the reference; prints, per other file, the first differing step and
   which fields differ (all rng game ball team0 team1 p0..p9), how many steps differ, and whether the runs re-converge.
   Exit status 0 when every file equals the reference on all common steps (and has at least 100 steps), else 1."""
import sys, gzip
F = ["all", "rng", "game", "ball", "team0", "team1"] + ["p%d" % i for i in range(10)]
def load(path):
    d = {}
    with (gzip.open(path, "rt", errors="replace") if path.endswith(".gz") else open(path, errors="replace")) as f:
        for line in f:
            w = line.split()
            if len(w) != 1 + len(F):   # a cut last line
                continue
            d[int(w[0])] = w[1:]
    return d
def main(argv):
    if len(argv) < 3:
        print(__doc__); return 2
    ref = load(argv[1]); bad = 0
    print("%s: %d steps (%s..%s)" % (argv[1], len(ref), min(ref, default="-"), max(ref, default="-")))
    for p in argv[2:]:
        o = load(p)
        common = sorted(set(ref) & set(o))
        diffs = [s for s in common if ref[s] != o[s]]
        msg = "%s: %d steps, %d common, %d differ" % (p, len(o), len(common), len(diffs))
        if len(common) < 100:
            msg += "  TOO FEW COMMON STEPS"; bad += 1
        if diffs:
            bad += 1
            s = diffs[0]
            fields = [F[i] for i in range(len(F)) if ref[s][i] != o[s][i]]
            msg += "\n   first difference at step %d: fields %s" % (s, ",".join(fields))
            msg += "\n   ref %s\n   got %s" % (" ".join(ref[s]), " ".join(o[s]))
            tail = [x for x in common if x > diffs[-1]]
            msg += "\n   last differing step %d%s" % (diffs[-1], "; re-converges after it" if tail else "; differs to the end")
        else:
            msg += "  IDENTICAL"
        print(msg)
    return 1 if bad else 0
sys.exit(main(sys.argv))
