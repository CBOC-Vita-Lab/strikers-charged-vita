#!/usr/bin/env python3
"""csvwin.py: mean per frame of FRAMESTATS columns (MSC_VGX_FRAMESTATS csv, HASH=1 runs) over frame windows.
  csvwin.py <vgx_frames.csv> [a-b ...]   (default: the GXD profile windows)   [--cols prims,verts,...]"""
import csv, sys, argparse
ap = argparse.ArgumentParser(); ap.add_argument('csv'); ap.add_argument('win', nargs='*')
ap.add_argument('--cols', default='prims,primsDL,verts,drawCalls,streams,streamBytes,vertsDL,dlHit,dlBuild,dlRehash,dlBypass,staticDraws,staticVerts,softVerts,texBound,texHashes,texHashBytes,texUploads,vtxBytes,idxBytes,uniBytes,atlasPasses')
a = ap.parse_args()
wins = [tuple(map(int, w.split('-'))) for w in a.win] or [(600,640),(1100,1150),(1650,1700),(1780,1830),(2272,2293),(3000,3060)]
rows = list(csv.DictReader(open(a.csv)))
cols = a.cols.split(',')
print('%-10s' % 'window' + ''.join('%11s' % c[:10] for c in cols))
for lo, hi in wins:
    sel = [r for r in rows if r['frame'].isdigit() and lo <= int(r['frame']) <= hi]
    if not sel: continue
    print('%-10s' % ('%d-%d' % (lo, hi)) + ''.join('%11.1f' % (sum(float(r[c]) for r in sel) / len(sel)) for c in cols))
