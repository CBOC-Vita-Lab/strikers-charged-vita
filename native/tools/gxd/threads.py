#!/usr/bin/env python3
"""threads.py: per-scene instruction counts of EVERY thread of a qemu run made with tools/perf/run.sh or tools/gxd/run.sh (marks in err.log).

  threads.py <err.log> [--skip K] [--min-frames N] [--thread-names t0=game,t1=render,...]

Segments the run by the harness' '[nav] f=N scenes: [...]' / 'task state A -> B' lines (as tools/perf/report.py) and prints, per segment, the mean and
median M instructions per presented frame of each thread (t0 = the game thread; the others in the order qemu created them: with MSC_FRAMESEND=nofork
t1 = the render thread (D, the vgx player), later ones audio / helper lanes). Executed ARM instructions, not time."""
import re, sys, argparse, statistics
ap = argparse.ArgumentParser()
ap.add_argument('log'); ap.add_argument('--skip', type=int, default=3); ap.add_argument('--min-frames', type=int, default=15)
ap.add_argument('--from', dest='lo', type=int, default=0); ap.add_argument('--to', dest='hi', type=int, default=10**9)
a = ap.parse_args()
mark = re.compile(r'\[insn\] mark frame=(-?\d+) caller=\d+((?: t\d+=\d+)+)')
scene_re = re.compile(r'\[nav\] f=(\d+) scenes: \[(.*?)\](?: top=(\S+))?')
state_re = re.compile(r'\[nav\] f=(\d+) task state (\d+) -> (\d+)')
frames = {}; changes = []; cur_scene = '-'; cur_state = '0'
for line in open(a.log, errors='replace'):
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
ks = sorted(frames)
segs = []
last = None
for f, sc, st in changes:
    key = (sc, st)
    if key != last: segs.append([f, sc, st]); last = key
for i, s in enumerate(segs): s.append(segs[i + 1][0] - 1 if i + 1 < len(segs) else ks[-1])
print(f"{'scene [task state]':38} {'frames':>6} | " + ' | '.join(f'{"t%d mean  med  p95" % t:>22}' for t in range(6)))
for f0, sc, st, f1 in segs:
    ds = {}
    for f in range(max(f0 + a.skip, a.lo), min(f1, a.hi) + 1):
        if f in frames and f - 1 in frames:
            for t in range(min(len(frames[f]), len(frames[f - 1]))): ds.setdefault(t, []).append((frames[f][t] - frames[f - 1][t]) / 1e6)
    if not ds or len(ds[0]) < a.min_frames: continue
    out = []
    for t in range(6):
        if t in ds:
            x = sorted(ds[t]); out.append(f'{statistics.mean(x):7.3f} {x[len(x)//2]:6.3f} {x[min(len(x)-1, int(len(x)*0.95))]:6.3f}')
        else: out.append(' ' * 22)
    print(f"{(sc + ' [' + st + ']')[:38]:38} {len(ds[0]):6} | " + ' | '.join(out) + f"   (frames {f0}-{f1})")
