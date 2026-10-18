#!/usr/bin/env python3
"""make_fulltime30.py - tools/nav/nav_fulltime.txt for the default 30 fps presentation.

nav_fulltime.txt plays the match with 562 `hold <frames> ...` lines. Its own comment: "the 10800 frames of holds are one whole 3-minute match at 60 fps". The holds count PRESENTED frames,
so at MSC_FPS=30 (two refreshes per frame) they cover twice the game time: the first match ends in the middle of the holds, the post-game screens run under stale inputs, a rematch starts,
a second match is played and the series-complete pop-up (Rematch / Change teams / Quit) appears where the script expects the end of the first one (found 16 Oct 2026, W5 FPS).
This writes the same script with every hold's frame count halved (rounded up, at least 1): the same inputs over the same game time at 30 fps. Everything else (menus, post-game) is unchanged.
  python3 tools/fps/make_fulltime30.py tools/nav/nav_fulltime.txt > tools/fps/nav/nav_fulltime30.txt
"""
import re
import sys

out = []
for line in open(sys.argv[1]):
    m = re.match(r'^(hold)\s+(\d+)(\s.*)?$', line.rstrip('\n'))
    if m:
        n = max(1, (int(m.group(2)) + 1) // 2)
        out.append('hold %d%s\n' % (n, m.group(3) or ''))
    else:
        out.append(line)
sys.stdout.write('# nav_fulltime.txt with every hold halved (tools/fps/make_fulltime30.py): one whole match at 30 fps. Generated; edit the generator.\n')
sys.stdout.write(''.join(out))
