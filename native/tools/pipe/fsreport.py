#!/usr/bin/env python3
"""fsreport.py: per-scene instruction budget of a run with the frame-send worker (tools/perf/run.sh ... MSC_FRAMESEND=<mode> MSC_FS_MARKS=1).

  fsreport.py run.log [--skip K] [--min-frames N] [--json out.json] [--from F --to F]

Reads the plugin's "[insn] mark" lines (cumulative instruction counts of every guest thread at each mark, qemu-arm, NOT time):
  frame marks (value < 0x20000000)   the harness' frame hook, issued by the game thread right after the fork
  0x30000001 fork   0x30000002 join done   (game thread)      0x30000003 job begin   0x30000004 job end   (worker; the game thread
                                                              itself in the modes off / inline / serial)
and for every presented frame k computes, in M instructions:
  main    the game thread's instructions between the frame marks k-1 and k       = P + M of the model below  (core 0's load)
  W       the job's instructions (job begin .. job end)                           (the second core's load; in off/inline/serial the
                                                                                   same instructions run on the game thread)
  P       game thread between join k-1 and fork k      M   between fork k and join k
  crit    max(M_(k-1), W_(k-1)) + P_k : the instruction count of the critical path of one frame period when the job starts at the fork
          and both cores run alike (the recompiled split's measure, split/NOTES.md "ARM numbers": P + max(M, W))
  gates   how many of the joins were not at the task boundary (a join with M < 0.25 of the median M of the segment counts as early)
Segments are the scenes / task states of the harness' [nav] lines, as report.py has them. Mean / median / p95 / max per segment."""
import re, sys, argparse, statistics, collections

ap = argparse.ArgumentParser()
ap.add_argument('log'); ap.add_argument('--skip', type=int, default=3); ap.add_argument('--min-frames', type=int, default=20)
ap.add_argument('--json'); ap.add_argument('--from', dest='lo', type=int, default=0); ap.add_argument('--to', dest='hi', type=int, default=10**9)
ap.add_argument('--game', type=int, default=0)
a = ap.parse_args()
mark = re.compile(r'\[insn\] mark frame=(-?\d+) caller=(\d+)((?: t\d+=\d+)+)')
scene_re = re.compile(r'\[nav\] f=(\d+) scenes: \[(.*?)\](?: top=(\S+))?')
state_re = re.compile(r'\[nav\] f=(\d+) task state (\d+) -> (\d+)')
ft_re = re.compile(r'\[ft\] (\d+) (\d+) (\d+) (-?\d+)')

frames = {}                    # frame -> counts at its mark
fork = {}                      # frame number the fork belongs to (the first frame mark after it) -> counts at fork
ft, changes = {}, []
fork_order, join_order, job_order = [], [], []
jb = None                      # (worker vcpu, counts) at job begin
unbound = None
for line in open(a.log, errors='replace'):
    m = mark.search(line)
    if m:
        v = int(m.group(1)); caller = int(m.group(2)); c = [int(x.split('=')[1]) for x in m.group(3).split()]
        if v == 0x30000001:
            unbound = c
        elif v == 0x30000002:
            join_order.append(c)
        elif v == 0x30000003:
            jb = (caller, c)
        elif v == 0x30000004 and jb:
            job_order.append(c[jb[0]] - jb[1][jb[0]])
            jb = None
        elif 0 <= v < 0x20000000:
            frames[v] = c
            if unbound is not None:
                fork[v] = unbound; fork_order.append(v); unbound = None
        continue
    m = ft_re.match(line)
    if m: ft[int(m.group(1))] = (int(m.group(2)), int(m.group(4))); continue
    m = scene_re.match(line)
    if m: changes.append((int(m.group(1)), 'scene ' + (m.group(3) or m.group(2)))); continue
    m = state_re.match(line)
    if m: changes.append((int(m.group(1)), 'task state %#x' % int(m.group(3)))); continue

G = a.game
fr = sorted(frames)
if not fr: sys.exit('no frame marks')
fk = fork_order
J = {f: join_order[i] for i, f in enumerate(fk) if i < len(join_order)}
W = {f: job_order[i] for i, f in enumerate(fk) if i < len(job_order)}
M = 1e6
rows_all = {}
for i, f in enumerate(fk):
    if f - 1 not in frames or f not in frames: continue
    main = frames[f][G] - frames[f - 1][G]
    if f not in J or f not in W: continue
    # P_f = main between join of the previous window and this fork
    pf = fk[i - 1] if i > 0 else None
    if pf is None or pf not in J: continue
    P = fork[f][G] - J[pf][G]
    Mw = J[f][G] - fork[f][G]
    rows_all[f] = dict(main=main, P=P, M=Mw, W=W[f])
# critical path of the period ending at fork f: max(M_prev, W_prev) + P_f
for i, f in enumerate(fk):
    if f in rows_all and i > 0 and fk[i - 1] in rows_all:
        pr = rows_all[fk[i - 1]]
        rows_all[f]['crit'] = max(pr['M'], pr['W']) + rows_all[f]['P']
        # early join: window much shorter than the whole gap to the next fork
changes.sort()
def label_at(f):
    s, t = '-', '-'
    for g, l in changes:
        if g > f: break
        if l.startswith('scene'): s = l[6:]
        else: t = l[11:]
    return '%s [%s]' % (s, t)
segs, prev, start = [], None, fr[0]
for f in fr:
    l = label_at(f)
    if l != prev:
        if prev is not None: segs.append((prev, start, f - 1))
        prev, start = l, f
segs.append((prev, start, fr[-1]))
def st(xs):
    xs = sorted(xs); n = len(xs)
    return statistics.mean(xs), xs[n // 2], xs[min(n - 1, int(n * 0.95))], xs[-1]
print('%-40s %6s | %-27s | %-13s | %-27s | %6s' % ('scene [task state]', 'frames', 'main P+M: mean med p95 max', 'W mean p95', 'crit P+max(M,W): mean med p95 max', 'steps'))
out = []
for lab, s0, s1 in segs:
    fs = [f for f in range(s0 + a.skip, s1 + 1) if f in rows_all and 'crit' in rows_all[f] and a.lo <= f <= a.hi]
    if len(fs) < a.min_frames: continue
    mn = st([rows_all[f]['main'] for f in fs]); w = st([rows_all[f]['W'] for f in fs]); c = st([rows_all[f]['crit'] for f in fs])
    Ps = statistics.mean([rows_all[f]['P'] for f in fs]); Ms = statistics.mean([rows_all[f]['M'] for f in fs])
    stp = [ft[f][0] - ft[f - 1][0] for f in fs if f in ft and f - 1 in ft]
    medM = statistics.median([rows_all[f]['M'] for f in fs])
    early = sum(1 for f in fs if rows_all[f]['M'] < 0.25 * medM) if medM > 0 else 0
    print('%-40s %6d | %6.2f %5.2f %5.2f %6.2f | %5.2f %5.2f | %6.2f %5.2f %5.2f %6.2f | %5.2f  P=%.2f M=%.2f early=%d (frames %d-%d)' % (
        lab[:40], len(fs), mn[0] / M, mn[1] / M, mn[2] / M, mn[3] / M, w[0] / M, w[2] / M, c[0] / M, c[1] / M, c[2] / M, c[3] / M,
        statistics.mean(stp) if stp else 0, Ps / M, Ms / M, early, fs[0], fs[-1]))
    out.append(dict(label=lab, frames=len(fs), main=mn, W=w, crit=c, P=Ps, M=Ms, early=early, first=fs[0], last=fs[-1], steps=statistics.mean(stp) if stp else 0))
if a.json:
    import json
    json.dump(out, open(a.json, 'w'), indent=1)
