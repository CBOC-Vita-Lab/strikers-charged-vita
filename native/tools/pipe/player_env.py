#!/usr/bin/env python3
"""Prints the environment the Vita player build applies by default (platform/vita/src/vita_plan.cpp), one NAME=VALUE per line, for the host harness.

The lists are read from the source (kSpeed, kQuality and the few single defaults) so that a switch added to the player's defaults is in the host runs the
moment it is added; nothing here is a copy. The multisampling rung (MSC_VGX_MSAA=4) is set by code in Apply, so it is listed by name below.
  python3 -I player_env.py <vita_plan.cpp>
"""
import re
import sys

src = open(sys.argv[1], encoding="utf-8").read()
out = []
for name in ("kSpeed", "kQuality"):
    m = re.search(r"static const char\* const " + name + r"\[\]\[2\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        sys.exit("player_env: %s not found in %s" % (name, sys.argv[1]))
    out += re.findall(r'\{"(MSC_\w+)",\s*"([^"]*)"\}', m.group(1))
for key in ("MSC_AI_BUDGET", "MSC_GAME_FIXES", "MSC_LATE_WAIT", "MSC_VGX_RT"):
    m = re.search(r'setenv\("' + key + r'",\s*"([^"]*)",\s*0\)', src)
    if not m:
        sys.exit("player_env: default of %s not found" % key)
    out.append((key, m.group(1)))
if 'add("MSC_VGX_MSAA", "4")' in src:
    out.append(("MSC_VGX_MSAA", "4"))
seen = set()
for k, v in out:
    if k not in seen:
        seen.add(k)
        print("%s=%s" % (k, v))
