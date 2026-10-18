#!/usr/bin/env python3
"""SKIN: before/after table of two census runs (tools/skin/runh.sh) per scene: stream bytes (vertices + indices), draw calls, uniform bytes,
static vertices, decoded vertices (TAGCENSUS runs only), per presented frame.
   compare.py <before run dir> <after run dir> [--md]"""
import sys, os, csv, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scenes import nav_windows

def load(run):
    rows = {}
    with open(os.path.join(run, 'vgx_frames.csv')) as fh:
        for r in csv.DictReader(fh): rows[int(r['frame'])] = r
    dec = collections.Counter()
    for line in open(os.path.join(run, 'err.log'), errors='replace'):
        if line.startswith('[tagcensus]'):
            d = dict(kv.split('=') for kv in line.split()[1:]); dec[int(d['f'])] += int(d['dec'])
    return rows, dec

def main():
    a, b = sys.argv[1], sys.argv[2]; md = '--md' in sys.argv
    ra, da = load(a); rb, db = load(b)
    wins = nav_windows(os.path.join(a, 'err.log'))
    hdr = ['scene', 'frames', 'stream KB', '', 'draws', '', 'uniform KB', '', 'decoded verts', '']
    if md: print('| scene | frames | stream KB/frame before -> after | draws | uniform KB | decoded vertices |'); print('|---|---|---|---|---|---|')
    else: print('%-24s %11s %17s %11s %15s %15s' % ('scene', 'frames', 'stream KB', 'draws', 'uniform KB', 'decoded'))
    for n, x, y in wins:
        fa = [ra[f] for f in range(x, y) if f in ra]; fb = [rb[f] for f in range(x, y) if f in rb]
        if len(fa) < 3 or len(fb) < 3: continue
        m = lambda fr, c: sum(float(r[c]) for r in fr) / len(fr)
        sa = (m(fa, 'vtxBytes') + m(fa, 'idxBytes')) / 1024; sb = (m(fb, 'vtxBytes') + m(fb, 'idxBytes')) / 1024
        dca, dcb = m(fa, 'drawCalls'), m(fb, 'drawCalls'); ua, ub = m(fa, 'uniBytes') / 1024, m(fb, 'uniBytes') / 1024
        dea = sum(da[f] for f in range(x, y)) / (y - x); deb = sum(db[f] for f in range(x, y)) / (y - x)
        if sa < 1 and sb < 1: continue
        if md: print('| %s | %d-%d | %.0f -> %.0f | %.0f -> %.0f | %.0f -> %.0f | %s |' % (n, x, y, sa, sb, dca, dcb, ua, ub, ('%.0f -> %.0f' % (dea, deb)) if (dea or deb) else '-'))
        else: print('%-24s %5d-%5d %7.0f -> %6.0f %4.0f -> %4.0f %6.0f -> %6.0f %6.0f -> %6.0f' % (n[:24], x, y, sa, sb, dca, dcb, ua, ub, dea, deb))

if __name__ == '__main__': main()
