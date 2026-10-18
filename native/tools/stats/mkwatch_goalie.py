#!/usr/bin/env python3
"""MSC_WATCH string that samples both goalies (g_pTeams[t]->m_pPlayers[4]) every MSC_WATCH_EVERY frames, same expression on the native game and on
the recompiled oracle (guest memory).  usage: mkwatch_goalie.py native|oracle [--stats]   (--stats appends the per-player statistics of mkwatch.py)
Fields (Goalie offsets, include/Game/Goalie.h): state 0x328, prev 0x32C, substate 0x334, wait 0x350, target time 0x3C4, ball owner 0x310, pos 0x30/0x34."""
import sys, subprocess, os
kind = sys.argv[1] if len(sys.argv) > 1 else "native"
teams = "g_pTeams" if kind == "native" else "806E0DF8"
F = [("st", 0x328, "s32"), ("prev", 0x32C, "s32"), ("sub", 0x334, "s32"), ("ball", 0x310, "u32"), ("x", 0x30, "f32"), ("y", 0x34, "f32"),
     ("wait", 0x350, "f32"), ("ttime", 0x3C4, "f32")]
out = []
for t in range(2):
    for n, off, ty in F:
        out.append("gk%d_%s:%s:[[%s+%d]+0xB4]+%X" % (t, n, ty, teams, 4 * t, off))
s = ",".join(out)
if "--stats" in sys.argv:
    s += "," + subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "mkwatch.py"), kind], capture_output=True, text=True).stdout.strip()
print(s)
