#!/usr/bin/env python3
"""SKIN: per-scene attribution of the renderer's per-frame stream by material program (MSC_VGX_TAGCENSUS=1 + MSC_SKIN_STATS=1 run).
   tags.py <run dir> [--min KB] [--windows name:a-b,...]
Columns per (scene, tag): stream draws, stream vertices, vertex KB, index KB, static draws, static vertices, decoded vertices (per frame).
Flags: 1 = the packet was CPU-skinned, 2 = GPU-skinned, 0 = plain; tag 00000000 = draws outside packet draws (FE immediate, effects ...)."""
import sys, re, os, glob, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scenes import nav_windows

def prog_names():
    names = {}
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../src')
    for f in glob.glob(os.path.join(src, 'NL/glx/*.cpp')) + glob.glob(os.path.join(src, 'Game/**/*.cpp'), recursive=True):
        try: t = open(f, errors='replace').read()
        except Exception: continue
        for m in re.finditer(r'programHash\s*=\s*0x([0-9A-Fa-f]+)', t):
            names[int(m.group(1), 16)] = os.path.basename(f).replace('MaterialProgram.cpp', '').replace('.cpp', '')
    return names

def main():
    run = sys.argv[1]; a = sys.argv[2:]; minkb = 1.0; wins = None
    while a:
        if a[0] == '--min': minkb = float(a[1]); a = a[2:]
        elif a[0] == '--windows':
            wins = [];
            for w in a[1].split(','):
                n, r = w.split(':'); x, y = r.split('-'); wins.append((n, int(x), int(y)))
            a = a[2:]
        else: break
    names = prog_names()
    per = collections.defaultdict(lambda: collections.defaultdict(lambda: [0] * 7))   # frame -> (tag, fl) -> counters
    skin = {}
    for line in open(os.path.join(run, 'err.log'), errors='replace'):
        if line.startswith('[tagcensus]'):
            d = dict(kv.split('=') for kv in line.split()[1:])
            f = int(d['f']); k = (int(d['tag'], 16), int(d['fl']))
            c = per[f][k]
            for i, n in enumerate(('sd', 'sv', 'sb', 'ib', 'td', 'tv', 'dec')): c[i] += int(d[n])
        elif line.startswith('[skinstat]'):
            d = dict(kv.split('=') for kv in line.split()[1:])
            skin[int(d['f'])] = {k: int(v) for k, v in d.items()}
    if wins is None: wins = nav_windows(os.path.join(run, 'err.log'))
    # the game's frame number (glGetCurrentFrame) and the renderer's presented-frame number run in step in these runs; both are used as given
    for n, x, y in wins:
        nf = y - x
        if nf <= 0: continue
        tot = collections.defaultdict(lambda: [0] * 7)
        for f in range(x, y):
            for k, c in per.get(f, {}).items():
                t = tot[k]
                for i in range(7): t[i] += c[i]
        allb = sum(t[2] + t[3] for t in tot.values()) / nf / 1024
        sk = [skin[f] for f in range(x, y) if f in skin]
        def sm(key): return sum(s.get(key, 0) for s in sk) / nf
        print('== %s frames %d-%d: stream %.0f KB/frame (vertices+indices); skin/frame: meshes %.1f packets %.1f cpu %.1f (%.0f verts, %.0f KB written) gpu %.1f (%.0f verts) morph meshes %.1f changed %.1f morph packets %.1f (%.0f verts) >32 bones %.1f'
              % (n, x, y, allb, sm('mesh'), sm('pk'), sm('cpk'), sm('cv'), sm('cb') / 1024, sm('gpk'), sm('gv'), sm('morph'), sm('mchg'), sm('mpk'), sm('mv'), sm('big')))
        print('   %-34s %3s %7s %8s %8s %7s %7s %8s %8s' % ('program', 'fl', 'sdraws', 'sverts', 'vtxKB', 'idxKB', 'tdraws', 'tverts', 'decoded'))
        for k, t in sorted(tot.items(), key=lambda kv: -(kv[1][2] + kv[1][3])):
            if (t[2] + t[3]) / nf / 1024 < minkb and t[6] / nf < 500: continue
            nm = names.get(k[0], '%08X' % k[0]) if k[0] else '(outside packets)'
            print('   %-34s %3d %7.1f %8.0f %8.1f %7.1f %7.1f %8.0f %8.0f' % (nm[:34], k[1], t[0] / nf, t[1] / nf, t[2] / nf / 1024, t[3] / nf / 1024, t[4] / nf, t[5] / nf, t[6] / nf))

if __name__ == '__main__': main()
