#!/usr/bin/env python3
"""analyse.py - summarise the MSC_FPS proof runs (FPS.jobA.sh): one line per run, from the harness's virtual clock.

  python3 -I tools/fps/analyse.py <rundir> [name ...]        rundir = build/np/W5FPS/runs/a, name = mi-30 mv-60 ...  (default: every <name>.log)
  python3 -I tools/fps/analyse.py --cmp <rundir> <ref name> <name> [name ...]      simulation digests (NP_DET_TRACE) of the runs against the reference, by step

Per run (all times are the virtual clock, 59.94 retraces per second):
  frames, frame lengths in refreshes 1/2/3/4+ (the retrace counter's growth between two [ft] lines), the mean length;
  steps/s: simulation steps per virtual second over the whole run and over the open-play frames (game state 5): must be 50 at every rate and mode switch
  [pace]: original frames per virtual second and rolls / changes per original frame (the six per-drawn-frame sites: must not depend on the rate)
  [fps]: the auto switches (time, direction), the movie line, the summary line the game prints at exit
"""
import gzip
import os
import re
import sys

REFRESH = 1001.0 / 60000.0
FT = re.compile(r'^\[ft\] (\d+) (\d+) (\d+) (-?\d+)')
PACE = re.compile(r'\[pace\] t=([\d.]+) drawn=(\d+) original=(\d+) rolls=(\d+) changes=(\d+)')


def lines(path):
    op = gzip.open if path.endswith('.gz') else open
    with op(path, 'rt', errors='replace') as f:
        for line in f:
            yield line


def run_summary(path):
    frames = []   # (frame, steps, retrace, state)
    pace = None
    switches, movie, summary, fps_lines, rc = [], [], '', [], ''
    for line in lines(path):
        m = FT.match(line)
        if m:
            frames.append(tuple(int(x) for x in m.groups()))
            continue
        m = PACE.search(line)
        if m:
            pace = (float(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)))
            continue
        if line.startswith('[fps] auto:'):
            switches.append(line.strip())
        elif line.startswith('[fps] movie:'):
            movie.append(line.strip())
        elif line.startswith('[fps] summary:'):
            summary = line.strip()
        elif line.startswith('rc='):
            rc = line.strip()
    out = {'name': os.path.basename(path).split('.')[0], 'rc': rc, 'frames': len(frames), 'switches': switches, 'movie': movie, 'summary': summary}
    if len(frames) > 2:
        hist = [0, 0, 0, 0]
        total = 0
        for a, b in zip(frames, frames[1:]):
            d = (b[2] - a[2]) & 0xFFFFFFFF
            hist[min(d, 4) - 1 if d >= 1 else 0] += 1
            total += d
        out['hist'] = hist
        out['mean_ms'] = total * REFRESH * 1000.0 / (len(frames) - 1)
        dr = (frames[-1][2] - frames[0][2]) * REFRESH
        out['steps_per_s'] = (frames[-1][1] - frames[0][1]) / dr if dr > 0 else 0.0
        # open play (state 5): windows of consecutive frames in that state
        s = r = 0
        for a, b in zip(frames, frames[1:]):
            if a[3] == 5 and b[3] == 5:
                s += b[1] - a[1]
                r += (b[2] - a[2]) & 0xFFFFFFFF
        out['open_steps_per_s'] = s / (r * REFRESH) if r else None
        out['open_frames'] = sum(1 for a in frames if a[3] == 5)
        # frame length histogram in open play only
        h5 = [0, 0, 0, 0]
        for a, b in zip(frames, frames[1:]):
            if a[3] == 5 and b[3] == 5:
                d = (b[2] - a[2]) & 0xFFFFFFFF
                h5[min(d, 4) - 1 if d >= 1 else 0] += 1
        out['open_hist'] = h5
        # one-second windows of open play: the steps the simulation ran in each (a window in which it ran is 50 +-5 at every rate; a window with 0 is a phase in
        # which it does not run (a replay, a pause), the rest are the windows that contain such a start or stop)
        t0 = frames[0][2]
        win = {}
        for a, b in zip(frames, frames[1:]):
            if a[3] != 5:
                continue
            w = int((((a[2] - t0) & 0xFFFFFFFF) * REFRESH))
            e = win.setdefault(w, [0, 0.0])
            e[0] += b[1] - a[1]
            e[1] += ((b[2] - a[2]) & 0xFFFFFFFF) * REFRESH
        bins = [0, 0, 0, 0]   # zero steps, 1-44, 45-55, 56+ per second
        for w, (st, dt) in win.items():
            if dt < 0.9:
                continue
            r = st / dt
            bins[0 if st == 0 else 1 if r < 45 else 2 if r <= 55 else 3] += 1
        out['windows'] = bins
    if pace:
        t, drawn, orig, rolls, changes = pace
        out['pace'] = 'virt %.1f s, drawn %d (%.2f/s), original %d (%.2f/s), rolls/orig %.4f, changes/orig %.4f' % (t, drawn, drawn / t if t else 0, orig, orig / t if t else 0, rolls / max(orig, 1), changes / max(orig, 1))
    return out


def show(o):
    print('%-14s %s frames=%d' % (o['name'], o['rc'], o['frames']))
    if 'hist' in o:
        print('   frame length in refreshes 1/2/3/4+: %s  mean %.2f ms; steps/s %.3f (whole run), %s (open play, %d frames, lengths %s)' % (
            '/'.join(map(str, o['hist'])), o['mean_ms'], o['steps_per_s'], ('%.3f' % o['open_steps_per_s']) if o['open_steps_per_s'] else '-', o['open_frames'], '/'.join(map(str, o['open_hist']))))
    if 'windows' in o:
        w = o['windows']
        print('   open play, 1 s windows by steps/s  0 / 1-44 / 45-55 / 56+ : %d / %d / %d / %d' % tuple(w))
    if 'pace' in o:
        print('   pace sites: ' + o['pace'])
    for s in o['switches'][:12]:
        print('   ' + s[:230])
    if len(o['switches']) > 12:
        print('   ... %d switches in all' % len(o['switches']))
    for s in o['movie']:
        print('   ' + s)
    if o['summary']:
        print('   ' + o['summary'][:300])


def load_trace(path):
    d = {}
    op = gzip.open if path.endswith('.gz') else open
    with op(path, 'rt', errors='replace') as f:
        for line in f:
            w = line.split()
            if len(w) == 17:
                d[int(w[0])] = w[1:]
    return d


def compare(d, ref, names):
    F = ['all', 'rng', 'game', 'ball', 'team0', 'team1'] + ['p%d' % i for i in range(10)]
    a = load_trace(os.path.join(d, ref + '.trace'))
    print('reference %s: %d steps' % (ref, len(a)))
    for n in names:
        p = os.path.join(d, n + '.trace')
        if not os.path.exists(p):
            print('%-14s no trace' % n)
            continue
        b = load_trace(p)
        common = sorted(set(a) & set(b))
        diffs = [s for s in common if a[s] != b[s]]
        msg = '%-14s %d steps, %d common with %s, %d differ' % (n, len(b), len(common), ref, len(diffs))
        if diffs:
            s = diffs[0]
            msg += '; first difference at step %d in fields %s' % (s, ','.join(F[i] for i in range(16) if a[s][i] != b[s][i]))
        elif common:
            msg += '; IDENTICAL on every common step'
        print(msg)


def main(argv):
    if len(argv) >= 2 and argv[1] == '--cmp':
        compare(argv[2], argv[3], argv[4:])
        return 0
    d = argv[1]
    names = argv[2:] or sorted(f[:-4] for f in os.listdir(d) if f.endswith('.log'))
    for n in names:
        p = os.path.join(d, n + '.log')
        if os.path.exists(p):
            show(run_summary(p))
    return 0


sys.exit(main(sys.argv))
