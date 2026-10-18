#!/usr/bin/env python3
"""points.py: script points of a nav run, from its log.

  points.py RUN.log [--off N] [--list] [--dedup]

Reads the harness lines of a run ("[nav] f=N scenes: [...] top=NAME", "[nav] f=N overlays: [...] top=NAME", "[nav] f=N match state A -> B",
"[script] f=N STATE ...") and prints  label frame  for every change of scene / overlay / match state / script marker. With --off N the frame is
shifted by N (to let a transition settle); --list prints a comma list of frames (MSC_DUMP_LIST) instead.
Stdlib only (run with python3 -I).
"""
import re, sys

def points(path, off=0):
    out = []
    last = {}
    for line in open(path, errors='replace'):
        m = re.match(r'\[nav\] f=(\d+) scenes: \[.*\] top=(\S+)', line)
        if m: kind, name, f = 'scene', m.group(2), int(m.group(1))
        else:
            m = re.match(r'\[nav\] f=(\d+) overlays: \[.*\] top=(\S+)', line)
            if m: kind, name, f = 'overlay', m.group(2), int(m.group(1))
            else:
                m = re.match(r'\[nav\] f=(\d+) match state (-?\d+) -> (-?\d+)', line)
                if m: kind, name, f = 'match', 'state%s' % m.group(3), int(m.group(1))
                else:
                    m = re.match(r'\[script\] f=(\d+) (.*)', line)
                    if m: kind, name, f = 'script', re.sub(r'\W+', '_', m.group(2).strip())[:40], int(m.group(1))
                    else: continue
        if last.get(kind) == name and kind != 'match': continue
        last[kind] = name
        out.append((kind + ':' + name, f + off))
    return out

if __name__ == '__main__':
    av = sys.argv[1:]
    off = 0
    if '--off' in av:
        i = av.index('--off'); off = int(av[i + 1]); del av[i:i + 2]
    lst = '--list' in av
    if lst: av.remove('--list')
    pts = points(av[0], off)
    if lst: print(','.join(str(f) for _, f in pts))
    else:
        for n, f in pts: print(n, f)
