#!/usr/bin/env python3
"""W5-SEED: summary of the recorded runs (build/np/W5SEED/rec/*.shl, *.done) as markdown tables.

  report.py [--rec DIR] [--base old.shl] [--out union.shl]  [--groups]

Per run: shaders / programs / variants it used and how many were NEW when the runs are taken in name order (saturation); per group (the prefix of the run
name: ai, hu, lg, ho, dm, cap, ft, cheat, sc, menu, ...): the keys of its runs, the keys found ONLY in that group, the keys new over the base list.
The union (+ the base list) is written with --out.
"""
import argparse, glob, os, sys, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shl


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rec', default='/home/claude/msc/build/np/W5SEED/rec')
    ap.add_argument('--base')
    ap.add_argument('--out')
    ap.add_argument('--exclude', default='^ho_', help='regex of run names left out of the union (the held-out set)')
    ap.add_argument('--runs', action='store_true', help='one line per run')
    a = ap.parse_args()
    import re
    ex = re.compile(a.exclude) if a.exclude else None
    runs = []
    for f in sorted(glob.glob(os.path.join(a.rec, '*.shl'))):
        n = os.path.basename(f)[:-4]
        st = ''
        d = os.path.join(a.rec, n + '.done')
        if os.path.exists(d):
            st = open(d).read().strip()
        try:
            L = shl.read(f)
        except Exception as e:
            continue
        if not L.vs and not L.fs:
            continue
        runs.append((n, st, L))
    base = shl.read(a.base) if a.base else None
    U = shl.merge([base]) if base else shl.SList()
    tot = lambda L: (len(L.vs), len(L.fs), len(L.prog), len(L.var))
    # per-run saturation in name order
    if a.runs:
        print('| run | status | vs | fs | prog | var | +vs | +fs | +prog | +var |\n|---|---|--:|--:|--:|--:|--:|--:|--:|--:|')
    X = shl.merge([base]) if base else shl.SList()
    for n, st, L in runs:
        if ex and ex.search(n):
            continue
        if X.vsSize is None:
            X.vsSize, X.fsSize, X.gen = L.vsSize, L.fsSize, L.gen
        nv = sum(1 for k in L.vs if X.add_vs(k)); nf = sum(1 for k in L.fs if X.add_fs(k))
        npg = sum(1 for t in L.prog if X.add_prog(*t)); nva = sum(1 for t in L.var if X.add_var(*t))
        if a.runs:
            print('| %s | %s | %d | %d | %d | %d | %d | %d | %d | %d |' % ((n, st) + tot(L) + (nv, nf, npg, nva)))
    # groups
    groups = collections.OrderedDict()
    for n, st, L in runs:
        g = re.match(r'[a-z]+', n).group(0)
        groups.setdefault(g, []).append((n, st, L))
    gl = {g: shl.merge([L for _, _, L in v]) for g, v in groups.items()}
    print('\n| group | runs | ok | vs | fs | prog | var | only here: vs | fs | prog | new over base: vs | fs | prog |\n|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|')
    for g, v in groups.items():
        others = shl.merge([gl[o] for o in gl if o != g and not (ex and ex.search(o + '_'))]) if len(gl) > 1 else shl.SList()
        G = gl[g]
        ov = sum(1 for k in G.vs if not others.has_vs(k)); of = sum(1 for k in G.fs if not others.has_fs(k)); op = sum(1 for t in G.prog if not others.has_prog(*t))
        if base:
            bv = sum(1 for k in G.vs if not base.has_vs(k)); bf = sum(1 for k in G.fs if not base.has_fs(k)); bp = sum(1 for t in G.prog if not base.has_prog(*t))
        else:
            bv = bf = bp = 0
        ok = sum(1 for _, st, _ in v if st == 'ok')
        print('| %s | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d |' % ((g, len(v), ok) + tot(G) + (ov, of, op, bv, bf, bp)))
    keep = [L for n, st, L in runs if not (ex and ex.search(n))]
    allU = shl.merge(([base] if base else []) + keep)
    print('\nunion%s: vs %d fs %d prog %d var %d' % (' (with the base)' if base else '', *tot(allU)))
    if a.out:
        shl.write(allU, a.out)
        print('written', a.out)


if __name__ == '__main__':
    main()
