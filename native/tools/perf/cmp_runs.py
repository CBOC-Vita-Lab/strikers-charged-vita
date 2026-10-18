#!/usr/bin/env python3
"""cmp_runs.py A B [C ...]: are two (or more) runs of the native game the same game?

  cmp_runs.py runs/a1-off runs/a1-thread [--max 10]

Compares, against the first directory: the simulation digests ("[hash] f=N sim=..." lines of err.log), the renderer's per-frame hash of
everything handed to the back end (vgx_frames.csv, last column: MSC_VGX_HASH=1), the harness' state-change lines ("[nav] ..." with their
frame numbers) and the final line of the run. Prints how many of each agree and the first differences."""
import sys, re, csv, os, argparse
ap = argparse.ArgumentParser(); ap.add_argument('dirs', nargs='+'); ap.add_argument('--max', type=int, default=8); ap.add_argument('--nav', action='store_true', help='compare [nav] lines too')
a = ap.parse_args()
def load(d):
    h, nav = {}, []
    for line in open(os.path.join(d, 'err.log'), errors='replace'):
        m = re.match(r'\[hash\] f=(\d+) sim=(\S+)', line)
        if m: h[int(m.group(1))] = m.group(2)
        elif line.startswith('[nav]'): nav.append(line.strip())
        elif line.startswith('nav] f='): nav.append('[' + line.strip())   # (the qemu plugin's output can cut into the guest's line on the shared stderr: the '[' went to the plugin's line)
    v = {}
    p = os.path.join(d, 'vgx_frames.csv')
    if os.path.exists(p):
        with open(p, newline='') as f:
            r = csv.reader(f); next(r)
            for row in r:
                if row: v[int(row[0])] = row[-1]
    return h, v, nav
base = load(a.dirs[0]); print('base', a.dirs[0], 'sim digests', len(base[0]), 'frame hashes', len(base[1]), 'nav lines', len(base[2]))
rc = 0
for d in a.dirs[1:]:
    h, v, nav = load(d)
    common = sorted(set(base[0]) & set(h)); diff = [f for f in common if base[0][f] != h[f]]
    vc = sorted(set(base[1]) & set(v)); vd = [f for f in vc if base[1][f] != v[f]]
    print('%s: sim digests %d/%d equal%s; frame hashes %d/%d equal%s' % (d, len(common) - len(diff), len(common), ' (first differing sample f=%d)' % diff[0] if diff else '',
          len(vc) - len(vd), len(vc), ' (first differing frame %d)' % vd[0] if vd else ''))
    if len(base[1]) != len(v): print('   frame counts differ: %d vs %d (last frames %d, %d)' % (len(base[1]), len(v), max(base[1] or [0]), max(v or [0])))
    if a.nav:
        n = sum(1 for x, y in zip(base[2], nav) if x == y)
        print('   [nav] lines equal %d/%d (lengths %d, %d)' % (n, min(len(base[2]), len(nav)), len(base[2]), len(nav)))
        k = 0
        for x, y in zip(base[2], nav):
            if x != y and k < a.max: print('     < ' + x[:150]); print('     > ' + y[:150]); k += 1
    for f in vd[:a.max]: print('   frame %d: %s vs %s' % (f, base[1][f], v[f]))
    if diff or vd: rc = 1
sys.exit(rc)
