#!/usr/bin/env python3
"""W5-SEED: splits the catalogue into the EAGER tier (what the warm-up at the first frame links) and the rest (linked from the seed's GXPs on first use).

  tiers.py [--rec DIR] [--base old.shl] [--share 0.5] [--frontend REGEX] [--match REGEX] [--out-core core.shl] [--out-full full.shl] [--table]

core = (every key the front-end runs used: boot, menus, scenes) + (every key used by at least SHARE of the match runs) + (the base list, which the console always had)
A program is eager when both its shaders are; variants follow their program. --table prints how big the core is for several shares.
"""
import argparse, glob, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shl


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rec', default='/home/claude/msc/build/np/W5SEED/rec')
    ap.add_argument('--base')
    ap.add_argument('--share', type=float, default=0.5)
    ap.add_argument('--frontend', default=r'^(sc\d*_|mn_|menu_|movie_|diag_|stadprobe_)')
    ap.add_argument('--match', default=r'^(ai_|hu_|lg_|lgr_|rnd_|wx_|cap_|cheat_|dm_|ft2_|mt_|pause_)')
    ap.add_argument('--exclude', default=r'^(ho_|hx_)')
    ap.add_argument('--out-core')
    ap.add_argument('--out-full')
    ap.add_argument('--table', action='store_true')
    a = ap.parse_args()
    fe, ma, ex = re.compile(a.frontend), re.compile(a.match), re.compile(a.exclude)
    runs = {}
    for f in sorted(glob.glob(os.path.join(a.rec, '*.shl'))):
        n = os.path.basename(f)[:-4]
        if ex.search(n):
            continue
        try:
            L = shl.read(f)
        except Exception:
            continue
        if L.vs or L.fs:
            runs[n] = L
    base = shl.read(a.base) if a.base else None
    full = shl.merge(([base] if base else []) + list(runs.values()))
    fel = [L for n, L in runs.items() if fe.search(n)]
    mal = [L for n, L in runs.items() if ma.search(n)]
    frontend = shl.merge(fel) if fel else shl.SList()
    if frontend.vsSize is None:
        frontend.vsSize, frontend.fsSize, frontend.gen = full.vsSize, full.fsSize, full.gen

    def core(share):
        C = shl.SList(); C.vsSize, C.fsSize, C.gen = full.vsSize, full.fsSize, full.gen
        cntv, cntf, cntp = {}, {}, {}
        for L in mal:
            for k in L.vs: cntv[k] = cntv.get(k, 0) + 1
            for k in L.fs: cntf[k] = cntf.get(k, 0) + 1
            for t in L.prog: cntp[t] = cntp.get(t, 0) + 1
        need = share * len(mal)
        okv = lambda k: frontend.has_vs(k) or cntv.get(k, 0) >= need or (base and base.has_vs(k))
        okf = lambda k: frontend.has_fs(k) or cntf.get(k, 0) >= need or (base and base.has_fs(k))
        progs = [t for t in full.prog if (frontend.has_prog(*t) or cntp.get(t, 0) >= need or (base and base.has_prog(*t)))]
        for v, f in progs:
            C.add_vs(v); C.add_fs(f); C.add_prog(v, f)
        for t in full.var:
            if C.has_prog(t[0], t[1]):
                C.add_var(*t)
        C.dups = 0
        return C
    print('catalogue: vs %d fs %d prog %d var %d  (%d runs, %d front-end, %d match)' % (len(full.vs), len(full.fs), len(full.prog), len(full.var), len(runs), len(fel), len(mal)))
    print('front-end alone: vs %d fs %d prog %d var %d' % (len(frontend.vs), len(frontend.fs), len(frontend.prog), len(frontend.var)))
    if base:
        print('base (old seed list): vs %d fs %d prog %d var %d' % (len(base.vs), len(base.fs), len(base.prog), len(base.var)))
    if a.table:
        print('| share of match runs | vs | fs | prog | var |\n|--:|--:|--:|--:|--:|')
        for sh in (0.0, 0.1, 0.25, 0.5, 0.75, 0.9, 1.0):
            C = core(sh)
            print('| %d%% | %d | %d | %d | %d |' % ((int(sh * 100),) + (len(C.vs), len(C.fs), len(C.prog), len(C.var))))
    C = core(a.share)
    print('core at %.0f%%: vs %d fs %d prog %d var %d' % ((a.share * 100,) + (len(C.vs), len(C.fs), len(C.prog), len(C.var))))
    if a.out_core:
        shl.write(C, a.out_core); print('written', a.out_core, os.path.getsize(a.out_core), 'bytes')
    if a.out_full:
        shl.write(full, a.out_full); print('written', a.out_full, os.path.getsize(a.out_full), 'bytes')


if __name__ == '__main__':
    main()
