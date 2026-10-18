#!/usr/bin/env python3
"""cmpthreads.py: per-scene M instructions per presented frame of the game thread (t0, includes W) and the render thread (t1 = D) of several qemu runs side by side.

  cmpthreads.py [--thread 1] [--min-frames 15] label=err.log label=err.log ...      (the first is the reference; the last column is the change last/first)

Segments exactly as threads.py (nav scene / task state lines); runs of the same script and the same simulation have the same segments."""
import re, sys, argparse, statistics
ap = argparse.ArgumentParser()
ap.add_argument('runs', nargs='+'); ap.add_argument('--thread', type=int, default=1)
ap.add_argument('--skip', type=int, default=3); ap.add_argument('--min-frames', type=int, default=15)
ap.add_argument('--md', action='store_true', help='Markdown table: scene, frames, D of each run, change, then the game thread (W+G) of the first and last run and its change')
a = ap.parse_args()
mark = re.compile(r'\[insn\] mark frame=(-?\d+) caller=\d+((?: t\d+=\d+)+)')
scene_re = re.compile(r'\[nav\] f=(\d+) scenes: \[(.*?)\](?: top=(\S+))?')
state_re = re.compile(r'\[nav\] f=(\d+) task state (\d+) -> (\d+)')
def load(log):
    frames = {}; changes = []; cur_scene = '-'; cur_state = '0'
    for line in open(log, errors='replace'):
        m = mark.search(line)
        if m:
            v = int(m.group(1))
            if 0 <= v < 0x20000000: frames[v] = [int(x.split('=')[1]) for x in m.group(2).split()]
            continue
        m = scene_re.search(line)
        if m:
            cur_scene = m.group(3) or m.group(2) or '-'; changes.append((int(m.group(1)), cur_scene, cur_state)); continue
        m = state_re.search(line)
        if m: cur_state = hex(int(m.group(3))); changes.append((int(m.group(1)), cur_scene, cur_state))
    ks = sorted(frames); segs = []; last = None
    for f, sc, st in changes:
        if (sc, st) != last: segs.append([f, sc, st]); last = (sc, st)
    for i, s in enumerate(segs): s.append(segs[i + 1][0] - 1 if i + 1 < len(segs) else ks[-1])
    out = []
    for f0, sc, st, f1 in segs:
        d = {0: [], a.thread: []}
        for f in range(f0 + a.skip, f1 + 1):
            if f in frames and f - 1 in frames:
                for t in d:
                    if t < len(frames[f]) and t < len(frames[f - 1]): d[t].append((frames[f][t] - frames[f - 1][t]) / 1e6)
        if len(d[0]) >= a.min_frames:
            out.append(((sc + ' [' + st + ']')[:34], f0, f1, len(d[0]), statistics.mean(d[0]), statistics.mean(d[a.thread]) if d[a.thread] else float('nan')))
    return out
labels = []; data = []
for r in a.runs:
    lab, _, path = r.partition('=')
    labels.append(lab); data.append(load(path))
ref = data[0]
if a.md:
    print('| scene | frames | ' + ' | '.join(f'D {l}' for l in labels) + ' | D change | game thread (W+G) ' + labels[0] + ' | ' + labels[-1] + ' | change (M) |')
    print('|---|---|' + '---|' * (len(labels) + 4))
    for row in ref:
        c1 = []; c0 = []
        for d in data:
            m = [x for x in d if x[1] == row[1]] or [x for x in d if x[0] == row[0] and abs(x[1] - row[1]) < 8]
            c1.append(m[0][5] if m else float('nan')); c0.append(m[0][4] if m else float('nan'))
        ch = (c1[-1] / c1[0] - 1) * 100 if c1[0] == c1[0] and c1[0] > 0 else float('nan')
        print(f'| {row[0]} | {row[3]} | ' + ' | '.join(f'{c:.2f}' for c in c1) + f' | {ch:+.0f} % | {c0[0]:.2f} | {c0[-1]:.2f} | {c0[-1]-c0[0]:+.2f} |')
    sys.exit(0)
print(f"{'scene':34} {'frames':>6} " + ' '.join(f'{l[:9]:>9}' for l in labels) + '   change(t%d)   |  t0 (game, incl. W): ' % a.thread + ' '.join(f'{l[:9]:>9}' for l in labels))
for i, row in enumerate(ref):
    cells1 = []; cells0 = []
    for d in data:
        m = [x for x in d if x[1] == row[1]] or [x for x in d if x[0] == row[0] and abs(x[1] - row[1]) < 8]
        cells1.append(m[0][5] if m else float('nan')); cells0.append(m[0][4] if m else float('nan'))
    ch = (cells1[-1] / cells1[0] - 1) * 100 if cells1[0] == cells1[0] and cells1[0] > 0 else float('nan')
    print(f'{row[0]:34} {row[3]:6} ' + ' '.join(f'{c:9.3f}' for c in cells1) + f'   {ch:+7.1f} %   |                       ' + ' '.join(f'{c:9.3f}' for c in cells0))
