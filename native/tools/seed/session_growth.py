#!/usr/bin/env python3
"""W5-SEED: how much of vitaGL's tables (1024 programs, 2048 shaders) and of the shader patcher a long play session uses.

  session_growth.py [--rec DIR] [--match REGEX] [--core core.shl] [-k 1,2,3,5,10,20,50] [-n samples] [--seed S]

A session = the eager list (--core, linked at the first frame) plus k whole matches drawn at random from the finished null runs whose names match --match.
Counts the distinct programs (vs, fs), shaders (vs + fs), fragment combos (fs, blend) and vertex combos (vs, layout), i.e. what the lazy-link console would hold at the end.
"""
import argparse, glob, os, random, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shl


def sets(path):
    L = shl.read(path)
    vs = {hash(k) for k in L.vs}
    fs = {hash(k) for k in L.fs}
    prog = {(hash(v), hash(f)) for v, f in L.prog}
    fc = {(hash(f), b) for v, f, lay, b in L.var}
    vc = {(hash(v), lay) for v, f, lay, b in L.var}
    return vs, fs, prog, fc, vc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rec', default='/home/claude/msc/build/np/W5SEED/rec')
    ap.add_argument('--match', default=r'^(ai_|hu_|cv_|cvp_|rnd_|ho_|hx_)')
    ap.add_argument('--core')
    ap.add_argument('-k', default='1,2,3,5,10,20,50')
    ap.add_argument('-n', type=int, default=300)
    ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()
    rx = re.compile(a.match)
    runs = []
    for p in sorted(glob.glob(a.rec + '/*.shl')):
        n = os.path.basename(p)[:-4]
        if rx.search(n) and os.path.exists(a.rec + '/' + n + '.done'):
            runs.append(sets(p))
    core = sets(a.core) if a.core else tuple(set() for _ in range(5))
    print('runs: %d matching %s; core: %s' % (len(runs), a.match, a.core or 'none'))
    print('| matches in the session | programs mean / p95 / max | shaders (vs+fs) mean / max | fragment combos (fs,blend) mean / max | vertex combos (vs,layout) mean / max |')
    print('|--:|---|---|---|---|')
    rnd = random.Random(a.seed)
    for k in [int(x) for x in a.k.split(',')]:
        acc = [[] for _ in range(4)]
        for _ in range(a.n):
            pick = [runs[rnd.randrange(len(runs))] for _ in range(k)]
            u = [set(c) for c in core]
            for r in pick:
                for i in range(5):
                    u[i] |= r[i]
            acc[0].append(len(u[2])); acc[1].append(len(u[0]) + len(u[1])); acc[2].append(len(u[3])); acc[3].append(len(u[4]))
        def m(x): return sum(x) / len(x)
        def p95(x): return sorted(x)[int(len(x) * 0.95)]
        print('| %d | %.0f / %d / %d | %.0f / %d | %.0f / %d | %.0f / %d |' % (k, m(acc[0]), p95(acc[0]), max(acc[0]), m(acc[1]), max(acc[1]), m(acc[2]), max(acc[2]), m(acc[3]), max(acc[3])))


if __name__ == '__main__':
    main()
