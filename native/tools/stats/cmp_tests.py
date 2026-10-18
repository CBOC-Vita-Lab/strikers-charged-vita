#!/usr/bin/env python3
"""cmp_tests.py A=glob B=glob : native against oracle over many seeds, one row per statistic: mean +- sd of each group, Mann-Whitney U and Welch t p-values.
Statistics: per team the final match statistics of the last [watch] line (st<team>_<player>_<name>, summed over the 5 players; goalie = player 4 separately for hits),
the score, and (when the log has the goalie samples of mkwatch_goalie.py) the share of the play samples (gs=5/6) each goalie state occupies.
usage: cmp_tests.py native='runs/tr1_*.log' oracle='runs/or_*.log' [--min-p 0.01]"""
import sys, glob, re, collections, math
import numpy as np
from scipy import stats as st

def parse(path):
    last = None; samples = []
    for line in open(path, errors="replace"):
        if not line.startswith("[watch]"): continue
        if "st0_0_" in line: last = line
        if "gk0_st=" in line and (" gs=5" in line or " gs=6" in line): samples.append(line)
    if not last: return None
    f = collections.defaultdict(float)
    for m in re.finditer(r"st(\d)_(\d)_(\w+)=(-?\d+|0x[0-9A-Fa-f]+)", last):
        t, p, name, v = m.groups(); v = int(v, 0)
        f["team%s %s" % (t, name)] += v
        if p == "4" and name in ("hits", "passes", "intercepted"): f["goalie%s %s" % (t, name)] += v
    sc = re.search(r"score=(\d+)-(\d+)", last)
    if sc: f["team0 SCORE"] = int(sc.group(1)); f["team1 SCORE"] = int(sc.group(2))
    if samples:
        n = len(samples)
        for t in (0, 1):
            cnt = collections.Counter(); ball = 0
            for l in samples:
                m = re.search(r"gk%d_st=(-?\d+)" % t, l)
                if m: cnt[int(m.group(1))] += 1
                b = re.search(r"gk%d_ball=(0x[0-9A-Fa-f]+|\d+)" % t, l)
                if b and int(b.group(1), 0): ball += 1
            for s_ in range(38): f["gk%d state %02d share %%" % (t, s_)] = 100.0 * cnt.get(s_, 0) / n
            f["gk%d has ball %%" % t] = 100.0 * ball / n
    return f

groups = []
args = [a for a in sys.argv[1:] if not a.startswith("--")]
minp = 1.0
for i, a in enumerate(sys.argv):
    if a == "--min-p": minp = float(sys.argv[i + 1]); args = [x for x in args if x != sys.argv[i + 1]]
for a in args:
    label, pat = a.split("=", 1)
    runs = [r for r in (parse(p) for p in sorted(glob.glob(pat))) if r]
    groups.append((label, runs))
(la, ra), (lb, rb) = groups[0], groups[1]
keys = sorted(set(k for r in ra + rb for k in r), key=lambda k: (k.split()[0] not in ("team0", "team1"), k))
print("%-34s %20s %20s %9s %9s" % ("statistic", "%s (n=%d)" % (la, len(ra)), "%s (n=%d)" % (lb, len(rb)), "MWU p", "Welch p"))
nsig = 0; ntest = 0
for k in keys:
    xa = np.array([r.get(k, 0.0) for r in ra]); xb = np.array([r.get(k, 0.0) for r in rb])
    if xa.size < 2 or xb.size < 2: continue
    if np.all(xa == xa[0]) and np.all(xb == xb[0]) and xa[0] == xb[0]: continue
    try: p1 = st.mannwhitneyu(xa, xb, alternative="two-sided").pvalue
    except Exception: p1 = float("nan")
    try: p2 = st.ttest_ind(xa, xb, equal_var=False).pvalue
    except Exception: p2 = float("nan")
    ntest += 1
    if p1 < 0.05: nsig += 1
    if min(p1, p2) <= minp:
        print("%-34s %9.2f +-%7.2f %9.2f +-%7.2f %9.3f %9.3f %s" % (k, xa.mean(), xa.std(ddof=1), xb.mean(), xb.std(ddof=1), p1, p2, "*" if p1 < 0.05 else ""))
print("%d statistics compared, %d with Mann-Whitney p < 0.05 (expected by chance: about %.1f)" % (ntest, nsig, 0.05 * ntest))
