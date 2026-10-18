#!/usr/bin/env python3
"""goalie_prof.py <log>... : goalie behaviour profile of whole-match logs.
 - from the native NP_GOALIE_TRACE lines ("[gt] sideS n=N st=A sub=B ball=C ..."): per side and state, entries, total sim steps, longest stay; hits by the goalie
 - from MSC_WATCH goalie samples (mkwatch_goalie.py: gkT_st, gkT_ball ...; native and oracle): share of the samples (play only) per state, per side
usage: goalie_prof.py [--watch] log... """
import re, sys, collections
NAMES = {0:"MOVE",1:"MOVE_WB",2:"SAVE_SETUP",3:"SAVE_REPOS",4:"SAVE",5:"MISS_CHIP",6:"DIVE_REC",7:"STS_REC",8:"PASS",9:"PASS_INT",10:"PRE_CROUCH",
         11:"PURSUE_CARRIER",12:"PURSUE_POUNCE",13:"U13",14:"LB_SETUP",15:"LB_CATCH",16:"LB_PICKUP",17:"LB_BOUNCING",18:"LB_ROLLING",19:"LB_DESPERATE",
         20:"LOB_SAVE",21:"U21",22:"OFFPLAY",23:"SNAP_BALL",24:"GRAB_BALL",25:"U25",26:"MEGA_STRIKE",27:"ELECTROCUTION",28:"FROZEN",29:"U29",30:"U30",
         31:"GRAB_MONTY",32:"U32",33:"U33",34:"STS_ATK_SETUP",35:"STS_ATK",36:"U36",37:"U37"}
def trace(path):
    ent = collections.defaultdict(lambda: [0, 0, 0]); hits = collections.Counter(); last = {}
    for l in open(path, errors="replace"):
        if not l.startswith("[gt] "): continue
        m = re.match(r"\[gt\] HIT side(\d) n=(\d+) st=(\d+)", l)
        if m: hits[(int(m.group(1)), int(m.group(3)))] += 1; continue
        m = re.match(r"\[gt\] side(\d) n=(\d+) st=(\d+) sub=(\d+) anim=(\d+) ball=(\d)", l)
        if not m: continue
        s, n, st = int(m.group(1)), int(m.group(2)), int(m.group(3))
        if s in last and last[s][0] != st:
            d = n - last[s][1]; e = ent[(s, last[s][0])]; e[0] += 1; e[1] += d; e[2] = max(e[2], d)
            last[s] = (st, n)
        elif s not in last: last[s] = (st, n)
    for s in last:
        st, n0 = last[s]
    return ent, hits
def watch(path):
    occ = collections.Counter(); tot = collections.Counter(); ballc = collections.Counter()
    for l in open(path, errors="replace"):
        if not l.startswith("[watch]") or "gk0_st=" not in l: continue
        if " gs=5" not in l and " gs=6" not in l: continue            # play / overtime
        for t in (0, 1):
            m = re.search(r"gk%d_st=(-?\d+)" % t, l); b = re.search(r"gk%d_ball=(0x[0-9A-Fa-f]+|\d+)" % t, l)
            if m:
                occ[(t, int(m.group(1)))] += 1; tot[t] += 1
                if b and int(b.group(1), 0): ballc[(t, int(m.group(1)))] += 1
    return occ, tot, ballc
args = [a for a in sys.argv[1:] if not a.startswith("--")]
for p in args:
    print("==", p)
    ent, hits = trace(p)
    if ent:
        print("  trace: side state: entries, total steps, longest stay")
        for (s, st), (c, tot, mx) in sorted(ent.items()):
            print("   side%d %-14s entries=%4d steps=%6d longest=%5d   goalie hits=%d" % (s, NAMES.get(st, st), c, tot, mx, hits.get((s, st), 0)))
        print("   goalie hits total per side:", {s: sum(v for (ss, _), v in hits.items() if ss == s) for s in (0, 1)})
    occ, tot, ballc = watch(p)
    if tot:
        for t in (0, 1):
            print("  watch side%d (%d play samples): " % (t, tot[t]) + ", ".join("%s %.0f%%" % (NAMES.get(st, st), 100.0 * c / tot[t]) for (tt, st), c in sorted(occ.items(), key=lambda kv: -kv[1]) if tt == t))
