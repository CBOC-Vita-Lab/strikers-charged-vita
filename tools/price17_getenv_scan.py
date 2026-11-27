#!/usr/bin/env python3
"""price17: list getenv sites not obviously cached (no 'static' on the line or the 2 lines above, not in a VGX_KNOB-style macro).
usage: price17_getenv_scan.py <worktree> [files...]   prints file:line text"""
import re,sys,glob,os
wt=sys.argv[1]; files=sys.argv[2:] or [f for pat in ("renderer/src/*.cpp","renderer/src/*.h","renderer/src/*.inc","renderer/vita/*.cpp","native/src/Game/**/*.cpp","native/platform/**/*.cpp","native/platform/**/*.h") for f in glob.glob(os.path.join(wt,pat),recursive=True)]
for f in sorted(files):
    L=open(f,errors="replace").read().split("\n")
    for i,l in enumerate(L):
        if "getenv" not in l or l.strip().startswith("//"): continue
        ctx=" ".join(L[max(0,i-2):i+1])
        cached = re.search(r"\bstatic\b",ctx) or "VGX_KNOB" in l
        print(("C " if cached else "U ")+os.path.relpath(f,wt)+":%d "%(i+1)+l.strip()[:150])
