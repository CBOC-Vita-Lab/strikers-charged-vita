#!/usr/bin/env python3
"""SKIN: per-scene means of the renderer's per-frame census (vgx_frames.csv) for a run of tools/skin/runh.sh.
   scenes.py <run dir> [--cols a,b,c] [--windows name:a-b,...]
Scenes are cut from the run's [nav] lines (task states and scenes); --windows overrides them."""
import sys, csv, re, os

def nav_windows(errlog):
    ev = []
    for line in open(errlog, errors='replace'):
        m = re.match(r'\[nav\] f=(\d+) (.*)', line)
        if m: ev.append((int(m.group(1)), m.group(2)))
    wins = []
    cur_scene = None; cur_task = None; start = None; name = None
    def close(f):
        if name is not None and start is not None and f > start: wins.append((name, start, f))
    task_names = {'16': 'NIS', '8': 'replay', '2': 'open play', '1': 'paused', '4': 'front end', '2097152': 'loading', '1048576': 'boot', '524288': 'intro'}
    for f, what in ev:
        m = re.match(r'scenes: \[.*\] top=(\S+)', what)
        t = re.match(r'task state (\d+) -> (\d+)', what)
        if m and cur_task == '4':
            close(f); name = m.group(1); start = f
        elif t:
            close(f); cur_task = t.group(2); name = task_names.get(cur_task, 'task' + cur_task); start = f
            if cur_task == '4': name = 'front end'
    return wins

def main():
    run = sys.argv[1]
    cols = 'drawCalls,verts,tris,vtxBytes,idxBytes,softVerts,vertsDL,dlHit,dlBuild,dlBypass,staticDraws,staticVerts,uniBytes'.split(',')
    wins = None
    a = sys.argv[2:]
    while a:
        if a[0] == '--cols': cols = a[1].split(','); a = a[2:]
        elif a[0] == '--windows':
            wins = []
            for w in a[1].split(','):
                n, r = w.split(':'); x, y = r.split('-'); wins.append((n, int(x), int(y)))
            a = a[2:]
        else: break
    rows = {}
    with open(os.path.join(run, 'vgx_frames.csv')) as fh:
        for r in csv.DictReader(fh): rows[int(r['frame'])] = r
    if wins is None: wins = nav_windows(os.path.join(run, 'err.log'))
    print('%-24s %11s ' % ('scene', 'frames') + ' '.join('%10s' % c[:10] for c in cols))
    for n, x, y in wins:
        fr = [rows[f] for f in range(x, y) if f in rows]
        if not fr: continue
        means = [sum(float(r[c]) for r in fr) / len(fr) for c in cols]
        print('%-24s %5d-%5d ' % (n[:24], x, y) + ' '.join('%10.0f' % m for m in means))

if __name__ == '__main__': main()
