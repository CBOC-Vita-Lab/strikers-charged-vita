#!/usr/bin/env python3
"""report.py: per-scene instruction budget of one qemu run (tools/perf/run.sh ... marks).

  report.py run.log [--game N] [--skip K] [--tasks] [--min-frames 20]

Segments the run into scenes with the harness' "[nav] f=N scenes: [id:name] top=Name" lines (MSC_WATCH=1) and the task state
("[nav] f=N task state A -> B"), and prints for every segment the game thread's M instructions per presented frame (mean, median,
p95, max), the other threads' total, the simulation steps per frame, and, where steps vary, the least-squares split
frame = P + M x steps. With --tasks also the per-task means (needs MSC_TASK_MARKS=1 in the run). Instruction counts of the
armhf guest under qemu-arm, not time. The first K frames of every segment are skipped (loading spike after a scene change)."""
import re, sys, argparse, statistics, collections

ap = argparse.ArgumentParser()
ap.add_argument('log'); ap.add_argument('--game', type=int, default=0); ap.add_argument('--skip', type=int, default=3)
ap.add_argument('--tasks', action='store_true'); ap.add_argument('--hist', action='store_true', help='cost by simulation steps in the frame'); ap.add_argument('--min-frames', type=int, default=20)
ap.add_argument('--json'); ap.add_argument('--from', dest='lo', type=int, default=0); ap.add_argument('--to', dest='hi', type=int, default=10**9)
a = ap.parse_args()
mark = re.compile(r'\[insn\] mark frame=(-?\d+) caller=(\d+)((?: t\d+=\d+)+)')
frames = {}            # n -> counts at the mark
tasks = collections.defaultdict(dict)   # frame(next mark) -> idx -> instr  (attributed to the frame whose mark follows the task end)
names, ft, changes = {}, {}, []
open_t, cur, last = {}, {}, None
scene_re = re.compile(r'\[nav\] f=(\d+) scenes: \[(.*?)\](?: top=(\S+))?')
state_re = re.compile(r'\[nav\] f=(\d+) task state (\d+) -> (\d+)')
cur_scene = '-'
for line in open(a.log, errors='replace'):
    m = mark.search(line)
    if m:
        v = int(m.group(1)); c = [int(x.split('=')[1]) for x in m.group(3).split()]
        if v >= 0x20000000:
            idx = (v - 0x20000000) >> 1
            if v & 1 == 0: open_t[idx] = c
            else:
                b = open_t.pop(idx, None)
                if b: cur[idx] = cur.get(idx, 0) + c[a.game] - b[a.game]
        elif v >= 0:
            frames[v] = c
            for idx, d in cur.items(): tasks[v][idx] = d
            cur = {}
        continue
    m = re.match(r'\[taskmarks\] (\d+)=(.*)', line)
    if m: names[int(m.group(1))] = m.group(2).strip(); continue
    m = re.match(r'\[ft\] (\d+) (\d+) (\d+) (-?\d+)', line)
    if m: ft[int(m.group(1))] = (int(m.group(2)), int(m.group(4))); continue
    m = scene_re.match(line)
    if m: changes.append((int(m.group(1)), 'scene ' + (m.group(3) or m.group(2)))); continue
    m = state_re.match(line)
    if m: changes.append((int(m.group(1)), 'task state %#x' % int(m.group(3)))); continue
ns = sorted(frames)
if not ns: sys.exit('no frame marks')
# segments: a new segment starts at every scene/state change (state changes inside a scene are shown in the label)
changes.sort()
labels = []
scene = '-'; state = '-'
seg_start = ns[0]; segs = []
bounds = sorted(set(f for f, _ in changes))
def label_at(f):
    s, t = '-', '-'
    for g, l in changes:
        if g > f: break
        if l.startswith('scene'): s = l[6:]
        else: t = l[11:]
    return '%s [%s]' % (s, t)
prev_label, start = None, ns[0]
for f in ns:
    l = label_at(f)
    if l != prev_label:
        if prev_label is not None: segs.append((prev_label, start, f - 1))
        prev_label, start = l, f
segs.append((prev_label, start, ns[-1]))
M = 1e6
def st(xs):
    xs = sorted(xs); n = len(xs)
    return statistics.mean(xs), xs[n // 2], xs[min(n - 1, int(n * 0.95))], xs[-1]
rows = []
print('%-44s %6s %8s %8s %8s %8s %8s %6s' % ('scene [task state]', 'frames', 'mean M', 'median', 'p95', 'max', 'other M', 'steps'))
for lab, s0, s1 in segs:
    fs = [f for f in range(s0 + a.skip, s1 + 1) if f in frames and (f - 1) in frames and a.lo <= f <= a.hi]
    if len(fs) < a.min_frames: continue
    g = [frames[f][a.game] - frames[f - 1][a.game] for f in fs]
    o = [sum(frames[f]) - frames[f][a.game] - sum(frames[f - 1]) + frames[f - 1][a.game] for f in fs]
    stp = [ft[f][0] - ft[f - 1][0] for f in fs if f in ft and f - 1 in ft]
    x = st(g)
    row = dict(label=lab, frames=len(fs), first=fs[0], last=fs[-1], mean=x[0], median=x[1], p95=x[2], max=x[3], other=statistics.mean(o), steps=statistics.mean(stp) if stp else 0)
    rows.append(row)
    print('%-44s %6d %8.3f %8.3f %8.3f %8.3f %8.3f %6.2f  (frames %d-%d)' % (lab[:44], len(fs), x[0] / M, x[1] / M, x[2] / M, x[3] / M,
          statistics.mean(o) / M, statistics.mean(stp) if stp else 0, fs[0], fs[-1]))
    if stp and len(set(stp)) > 1:
        # least squares: g = P + Mstep * steps
        n = min(len(stp), len(g)); xs = stp[:n]; ys = g[:n]
        mx, my = statistics.mean(xs), statistics.mean(ys)
        sxx = sum((u - mx) ** 2 for u in xs)
        if sxx > 0:
            b = sum((u - mx) * (w - my) for u, w in zip(xs, ys)) / sxx; p = my - b * mx
            row.update(P=p, M=b)
            print('%46s fit: frame = %.3f M + %.3f M x steps   (at 1.667 steps: %.3f M)' % ('', p / M, b / M, (p + 1.667 * b) / M))
    if a.hist and stp and len(set(stp)) > 1:
        by = collections.defaultdict(list)
        for f in fs:
            if f in ft and f - 1 in ft: by[ft[f][0] - ft[f - 1][0]].append(frames[f][a.game] - frames[f - 1][a.game])
        for k in sorted(by):
            y = st(by[k]); print('%46s %d steps: %4d frames (%2.0f%%)  mean %.3f  median %.3f  p95 %.3f  max %.3f' % ('', k, len(by[k]), 100.0 * len(by[k]) / len(stp), y[0] / M, y[1] / M, y[2] / M, y[3] / M))
    if a.tasks:
        ts = collections.defaultdict(list)
        for f in fs:
            for idx in names:
                ts[idx].append(tasks.get(f, {}).get(idx, 0))
        for idx in sorted(ts):
            if statistics.mean(ts[idx]) > 1000:
                print('%46s %-24s mean %.3f  p95 %.3f  max %.3f' % ('', names[idx], statistics.mean(ts[idx]) / M, st(ts[idx])[2] / M, max(ts[idx]) / M))

if a.json:
    import json
    json.dump(rows, open(a.json, 'w'), indent=1)
