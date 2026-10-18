#!/usr/bin/env python3
"""hot.py: instructions of the render thread (or game thread) inside one function, per source line (addr2line), from a framecount2 profile of a NON-PIE armhf exe.
  hot.py <prof.txt> <exe> <function-substring> <frames> [--thread other|game] [--top 40]"""
import argparse, subprocess, collections, re, bisect
ap = argparse.ArgumentParser()
ap.add_argument('prof'); ap.add_argument('exe'); ap.add_argument('fn'); ap.add_argument('frames', type=float)
ap.add_argument('--thread', default='other'); ap.add_argument('--top', type=int, default=40)
a = ap.parse_args()
out = subprocess.run(['arm-linux-gnueabihf-nm', '-n', '-S', '--defined-only', '-C', a.exe], capture_output=True, text=True).stdout
syms = []
for l in out.splitlines():
    p = l.split(None, 3)
    if len(p) == 4 and p[2] in 'Tt': syms.append((int(p[0], 16) & ~1, int(p[1], 16), p[3]))
ranges = [(s, s + z, n) for s, z, n in syms if a.fn in n]
if not ranges: raise SystemExit('no such function')
idx = 1 if a.thread == 'game' else 2
cnt = collections.Counter(); tot = 0
for l in open(a.prof):
    f = l.split(); pc = int(f[0], 16) & ~1; d = int(f[idx])
    if not d: continue
    for lo, hi, n in ranges:
        if lo <= pc < hi:
            cnt[pc] += d; tot += d; break
print(f'{a.fn}: {tot / a.frames / 1e6:.3f} M per frame ({len(ranges)} symbols: {", ".join(n[:60] for _,_,n in ranges[:3])})')
pcs = sorted(cnt)
# the profile has per-block counts at block-start pcs only: attribute the block to the line of its first pc and to the lines of the block's other instructions evenly is not
# possible, so list per block start with its source line.
a2l = subprocess.run(['arm-linux-gnueabihf-addr2line', '-e', a.exe, '-i', '-f', '-C'] , input='\n'.join('%x' % p for p in pcs), capture_output=True, text=True).stdout.splitlines()
# addr2line -i -f gives for each address: lines function/file:line pairs, inlined chain; no separator: use -a
a2l = subprocess.run(['arm-linux-gnueabihf-addr2line', '-a', '-e', a.exe, '-i', '-f', '-C'], input='\n'.join('%x' % p for p in pcs), capture_output=True, text=True).stdout.splitlines()
res = {}; cur = None
for l in a2l:
    if l.startswith('0x'):
        cur = int(l, 16); res[cur] = []; continue
    res[cur].append(l)
byline = collections.Counter()
for pc, d in cnt.items():
    r = res.get(pc, [])
    # r is a list of function, file:line, function, file:line ... innermost first
    chain = [r[i + 1].rsplit('/', 1)[-1] for i in range(0, len(r) - 1, 2)]
    byline[' <- '.join(chain[:3])] += d
for k, v in byline.most_common(a.top):
    print(f'  {v / a.frames / 1e6:7.3f} M {100 * v / tot:5.1f}%  {k}')
