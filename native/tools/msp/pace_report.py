#!/usr/bin/env python3
"""pace_report.py - summarise the frame-pacing proof runs (tools/msp/job_pace.sh).

  pace_report.py <rundir> [scene ...]

Reads <rundir>/<scene>-<cfg>.log (cfg A = 60 Hz, B = 30 fps pacing off, C = 30 fps pacing on; MSC_FRAME_PACING_LOG=2) and prints, per scene:
  - the virtual seconds, drawn frames and original frames the run covered, and the rates per virtual second
  - the discrete steps (emission rolls, texture-animation image changes and blinks) per virtual second and per ORIGINAL frame. Per second they do not
    tell the frame rates apart (an effect that is due emits once whatever the frame rate; only the wait for the roll changes); per original frame
    they do: the number of rolls per original frame is the mean number of effects waiting for their roll, which doubles at 30 fps when each roll
    covers two original frames of waiting, and returns to the 60 Hz value when pacing rolls once per original frame
  - the camera ease runs (start, duration, number of drawn frames) and the letterbox phases (time of each phase change)
All times are the harness's virtual clock (MSC_VIRTUAL_TIME=1), so they do not depend on the speed of the machine."""
import os
import re
import sys

PERIODIC = re.compile(r'\[pace\] t=([\d.]+) drawn=(\d+) original=(\d+) rolls=(\d+) changes=(\d+)')
EVENT = re.compile(r'\[pace-ev\] t=([\d.]+) drawn=(\d+) (.+?) (-?[\d.]+)\s*$')


def load(path):
    per, ev = [], []
    for line in open(path, errors='replace'):
        m = PERIODIC.search(line)
        if m:
            per.append((float(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5))))
            continue
        m = EVENT.search(line)
        if m:
            ev.append((float(m.group(1)), int(m.group(2)), m.group(3), float(m.group(4))))
    return per, ev


def eases(ev):
    """camera ease events -> runs (start t, duration, frames, final value); a run ends when the value stops rising or the gap exceeds 0.25 s."""
    runs = []
    cur = None
    for t, drawn, what, v in ev:
        if what != 'camera ease':
            continue
        if cur and t - cur[1] <= 0.25 and v >= cur[3]:
            cur[1] = t; cur[2] += 1; cur[3] = v
        else:
            if cur:
                runs.append(cur)
            cur = [t, t, 1, v]
    if cur:
        runs.append(cur)
    return [(r[0], r[1] - r[0], r[2], r[3]) for r in runs]


def main():
    d = sys.argv[1]
    scenes = sys.argv[2:] or sorted({f.rsplit('-', 1)[0] for f in os.listdir(d) if f.endswith('.log')})
    names = {'A': 'A 60 Hz', 'B': 'B 30 fps, pacing off', 'C': 'C 30 fps, pacing on'}
    for sc in scenes:
        print('== scene %s' % sc)
        print('%-22s %8s %8s %9s %8s %9s %9s %10s %11s %12s %8s' % ('run', 'virt s', 'drawn', 'original', 'drawn/s', 'orig/s', 'rolls/s', 'changes/s', 'rolls/orig', 'changes/orig', 'windows'))
        evs = {}
        for cfg in 'ABC':
            p = os.path.join(d, '%s-%s.log' % (sc, cfg))
            if not os.path.exists(p):
                continue
            per, ev = load(p)
            evs[cfg] = ev
            if not per:
                print('%-22s no [pace] lines' % names[cfg])
                continue
            t, dr, orig, rolls, ch = per[-1]
            print('%-22s %8.2f %8d %9d %8.2f %9.2f %9.3f %10.3f %11.4f %12.4f %8d' % (names[cfg], t, dr, orig, dr / t, orig / t, rolls / t, ch / t, rolls / max(orig, 1), ch / max(orig, 1), len(per)))
        for cfg in 'ABC':
            ev = evs.get(cfg)
            if not ev:
                continue
            runs = eases(ev)
            lb = [(t, w) for t, _, w, _ in ev if w.startswith('letterbox')]
            if runs:
                print('  %s camera ease runs (start t, virtual duration, drawn frames, end value): %s' % (cfg, '; '.join('%.2f %.3fs %df %.2f' % r for r in runs[:12])))
            if lb:
                print('  %s letterbox: %s' % (cfg, '; '.join('%.2f %s' % x for x in lb[:16])))
        print()


main()
