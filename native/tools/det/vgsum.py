#!/usr/bin/env python3
"""Summarise a valgrind memcheck log (tools/det/vg.sh) of the native game: uninitialised-value errors grouped by where they are used.
   tools/det/vgsum.py vg-k1.log [--top N] [--sim] [--origins]
   Groups by the innermost 3 frames of the error stack. --sim keeps only errors whose stack contains a simulation function
   (FixedUpdateTask::, cGame::, cFielder, Goalie, cBall, cTeam, Physics, dx, AI) -- the ones that can change a match.
   --origins prints the allocation site (with --track-origins=yes: 'Uninitialised value was created by a client request at ...')."""
import re, sys, collections
SIM = ("FixedUpdateTask", "cGame::", "cFielder", "Goalie", "cBall", "cTeam", "cPlayer", "cCharacter", "Physics", "dx", "Desire", "Fielder", "AIContext", "GoalieSave")
def main(argv):
    if len(argv) < 2:
        print(__doc__); return 2
    path = argv[1]; top = 25; sim = False; origins = False
    for i, a in enumerate(argv):
        if a == "--top": top = int(argv[i + 1])
        if a == "--sim": sim = True
        if a == "--origins": origins = True
    errs = []; cur = None; part = None
    for line in open(path, errors="replace"):
        m = re.match(r"==\d+== (.*)$", line.rstrip("\n"))
        if not m:
            continue
        t = m.group(1)
        if re.match(r"(Conditional jump|Use of uninitialised|Syscall param .* uninitialised|Invalid read|Invalid write)", t):
            cur = {"kind": t.split(" depends")[0].split(" of ")[0][:40], "frames": [], "origin": [], "orig_kind": ""}
            errs.append(cur); part = "frames"; continue
        if cur is None:
            continue
        if t.startswith(" Uninitialised value was created") or t.startswith(" Address 0x"):
            cur["orig_kind"] = t.strip(); part = "origin"; continue
        if t.strip() == "":
            part = None; continue
        fm = re.match(r"\s+(at|by) 0x[0-9A-Fa-f]+: (.*)$", t)
        if fm and part:
            fr = re.sub(r" \(in [^)]*\)", "", fm.group(2)).strip()
            cur[part if part in ("frames", "origin") else "frames"].append(fr)
    kept = [e for e in errs if e["frames"]]
    if sim:
        kept = [e for e in kept if any(s in f for f in e["frames"] for s in SIM)]
    groups = collections.OrderedDict()
    for e in kept:
        key = (e["kind"],) + tuple(e["frames"][:3])
        g = groups.setdefault(key, {"n": 0, "e": e}); g["n"] += 1
    print("%s: %d errors total, %d after filter, %d groups" % (path, len(errs), len(kept), len(groups)))
    for key, g in sorted(groups.items(), key=lambda kv: -kv[1]["n"])[:top]:
        e = g["e"]
        print("\n%5d x %s" % (g["n"], key[0]))
        for f in e["frames"][:8]:
            print("        " + f[:150])
        if origins and e["origin"]:
            print("      origin: " + e["orig_kind"][:100])
            for f in e["origin"][:6]:
                print("        " + f[:150])
    return 0
sys.exit(main(sys.argv))
