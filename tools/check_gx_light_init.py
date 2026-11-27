#!/usr/bin/env python3
"""Fails when game code declares a GXLightObj on the stack without initialising it. GXLoadLightObjImm sends all 64 bytes of the object
(the direction and the three pad words included), and the game never calls GXInitLightDir for these lights, so an uninitialised object puts
stack garbage into the GX stream (Galaxy finding, 13 Nov: it differs from turn to turn and breaks byte compares, stream caches and dedup).
Usage: python3 tools/check_gx_light_init.py [root]     (--stdin NAME reads one file's text from stdin, for the negative control)"""
import os, re, sys
PAT = re.compile(r"^\s*(?:const\s+)?GXLightObj\s+(\w+)\s*;", re.M)
def scan(text, name):
    return [f"{name}:{text.count(chr(10), 0, m.start()) + 1}: GXLightObj {m.group(1)}; is not initialised (write `= {{}}`)" for m in PAT.finditer(text)]
if "--stdin" in sys.argv:
    out = scan(sys.stdin.read(), sys.argv[sys.argv.index("--stdin") + 1])
else:
    root = next((a for a in sys.argv[1:] if not a.startswith("--")), os.getcwd()); out = []
    for base in ("native/src", "native/platform", "renderer/src"):
        for dp, dns, fns in os.walk(os.path.join(root, base)):
            dns[:] = [d for d in dns if d not in ("tests", "test", "tools")]
            for fn in fns:
                if fn.endswith((".cpp", ".c", ".h")):
                    p = os.path.join(dp, fn)
                    out += scan(open(p, errors="replace").read(), os.path.relpath(p, root))
print("\n".join(out) or "no uninitialised GXLightObj")
sys.exit(1 if out else 0)
