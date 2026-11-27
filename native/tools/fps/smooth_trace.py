#!/usr/bin/env python3
"""smooth_trace.py <trace> [from] [to]: read an MSC_SMOOTH_TRACE file (np_smooth.cpp, host or console) and say how evenly players, their limbs, the ball and the camera move
per picture against the time each picture represents. Live play only. Host runs show every picture for one refresh (virtual retrace), so the picture-to-picture time is the
frame's own (real_ms); evenness = |speed_k / speed_{k-1} - 1| for objects moving steadily (a player over 2 m/s, a limb bone over 0.3 m/s in model space, the ball over 3 m/s).
A 50 Hz simulation shown at 60 without the blend moves 0 in one picture of six: that is a ratio of -1 (or +inf after it), so the share over 0.5 shows it."""
import math, sys

def pct(a, p):
    if not a: return float('nan')
    a = sorted(a); return a[min(len(a) - 1, int(p * (len(a) - 1)))]

def main():
    path = sys.argv[1]; lo = int(sys.argv[2]) if len(sys.argv) > 2 else 0; hi = int(sys.argv[3]) if len(sys.argv) > 3 else 1 << 60
    rows = []
    for line in open(path):
        if line.startswith('#'): continue
        f = line.split()
        if len(f) < 17 + 60: continue
        fr = int(f[0])
        if fr < lo or fr > hi: continue
        rows.append(f)
    out = {'run': [], 'bone': [], 'ball': [], 'cam': [], 'rep': []}
    zero = {'run': 0, 'bone': 0}
    blends = []; steps = {}
    prev = None; pv = {}
    for f in rows:
        live = f[1] == '1'
        if not live: prev = None; pv = {}; continue
        real = float(f[5]); adv = float(f[6]); blend = float(f[7]); rep = float(f[8])
        blends.append(blend); steps[int(f[4])] = steps.get(int(f[4]), 0) + 1
        cam = list(map(float, f[11:14])); ball = list(map(float, f[14:17]))
        pos = [list(map(float, f[17 + 3 * i:20 + 3 * i])) for i in range(10)]
        bone = [list(map(float, f[47 + 3 * i:50 + 3 * i])) for i in range(10)]
        if prev is not None:
            dt = adv / 1000.0
            if dt > 0:
                out['rep'].append((rep - prev['rep']) * 1000.0 / adv - 1.0)
                def obj(key, a, b, vmin, vmax, tag):
                    if any(math.isnan(x) for x in a + b): pv.pop(tag, None); return
                    v = math.dist(a, b) / dt
                    if vmin < v < vmax and pv.get(tag, 0) > vmin: out[key].append(v / pv[tag] - 1.0)
                    if key in zero and pv.get(tag, 0) > vmin and v < 1e-6: zero[key] += 1
                    pv[tag] = v
                for i in range(10):
                    obj('run', pos[i], prev['pos'][i], 2.0, 15.0, 'p%d' % i)
                    obj('bone', bone[i], prev['bone'][i], 0.3, 20.0, 'b%d' % i)
                obj('ball', ball, prev['ball'], 3.0, 60.0, 'ball')
                obj('cam', cam, prev['cam'], 1.0, 40.0, 'cam')
        prev = {'rep': rep, 'cam': cam, 'ball': ball, 'pos': pos, 'bone': bone}
    print("pictures of live play %d; steps a picture %s; blend factor mean %.3f" % (len(blends), dict(sorted(steps.items())), sum(blends) / max(1, len(blends))))
    print("represented time a picture against the frame's time: |ratio-1| median %.4f p95 %.4f" % (pct([abs(x) for x in out['rep']], .5), pct([abs(x) for x in out['rep']], .95)))
    for k, name in (('run', 'running players (root)'), ('bone', 'limb bone (pose, model space)'), ('ball', 'ball'), ('cam', 'camera')):
        a = [abs(x) for x in out[k]]
        over = 100.0 * sum(1 for x in a if x > 0.5) / max(1, len(a))
        print("%-32s n %6d  |speed ratio-1| median %.3f p95 %.3f  share over 0.5 %.1f %%%s" % (name, len(a), pct(a, .5), pct(a, .95), over,
              ("  frozen pictures %d" % zero[k]) if k in zero else ""))

main()
