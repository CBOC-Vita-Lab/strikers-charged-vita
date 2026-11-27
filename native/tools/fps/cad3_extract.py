#!/usr/bin/env python3
"""Extract the held-scene picture-hold histograms of console logs into cad3_scenes.txt (the input of cad3_unit.cpp).

usage: cad3_extract.py OUT.txt LOG [LOG ...]

A console log prints, every 5 s, a "[present] measured" line with "held 0/1/2/3/4+ vblanks a/b/c/d/e": how many pictures were on screen for 0, 1, 2, 3, 4 or more refreshes. The
"[nav] f=N task state A -> B" lines say which scene was running (16 cut-scene, 8 replay, 131072 debug replay are the held ones). A window is assigned to the scene running when it was
printed (the first one of a scene also holds the end of the scene before it: a few pictures). Only counts are kept: no per-picture time exists in these logs.
Output lines: <log> <scene index in that log> <state> <window index> <n1> <n2> <n3> <n4plus>
"""
import re, sys, os
HELD = {16, 8, 131072}
nav = re.compile(r'^\[nav\] f=\d+ task state (\d+) -> (\d+)')
pres = re.compile(r'^\[present\] measured, .*? held 0/1/2/3/4\+ vblanks (\d+)/(\d+)/(\d+)/(\d+)/(\d+);')
def main(out, logs):
    rows = []
    for path in logs:
        name = os.path.basename(os.path.dirname(path)) + "/" + os.path.basename(path)
        state, scene, win = None, 0, 0
        wrote = False
        for line in open(path, errors='replace'):
            m = nav.match(line)
            if m:
                new = int(m.group(2))
                if new in HELD and new != state:
                    scene += 1; win = 0
                state = new
                continue
            m = pres.match(line)
            if m and state in HELD:
                win += 1
                n = [int(x) for x in m.groups()]
                rows.append("%s %d %d %d %d %d %d %d" % (name, scene, state, win, n[1], n[2], n[3], n[4]))
    open(out, 'w').write("# held-scene windows from console logs: log scene state window n1 n2 n3 n4+ (pictures held 1, 2, 3, 4+ refreshes); made by cad3_extract.py\n" + "\n".join(rows) + "\n")
    print(len(rows), "windows")
main(sys.argv[1], sys.argv[2:])
