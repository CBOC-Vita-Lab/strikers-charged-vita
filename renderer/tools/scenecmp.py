#!/usr/bin/env python3
"""scenecmp.py: drawsets.py for every scene of two nav-script runs (native game / recompiled build).

  scenecmp.py A_DIR A_LOG B_DIR B_LOG [--skip N] [--top K] [--only NAME]

The scene boundaries come from the harness's "[nav] f=N scenes: [...] top=NAME" lines of the two logs; segments with the same
name in the same order are paired (the first and last SKIP frames of a segment, default 10, are left out: transitions).
--combos: leave the vertex and index counts out of a draw's signature (dynamic meshes: text, particles, crowd).
Prints one line per scene (distinct draws, shared, only in A, only in B) and, with --top, the first differing signatures.
"""
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import drawsets  # noqa: E402


def segments(log):
    seq = []
    for line in open(log, errors='replace'):
        m = re.match(r'\[nav\] f=(\d+) (?:scenes: \[.*\] top=(\S+)|overlays: \[.*\] top=(\S+)|match object created)', line)
        if m:
            name = m.group(2) or m.group(3) or 'MatchObject'
            if not seq or seq[-1][1] != name:
                seq.append((int(m.group(1)), name))
    return seq


def main():
    av = sys.argv[1:]
    opt = {'skip': 10, 'top': 4, 'only': None}
    while '--sub' in av:
        i = av.index('--sub'); drawsets.SUBS.append(tuple(av[i + 1].split('=', 1))); del av[i:i + 2]
    if '--geo' in av:
        av.remove('--geo'); drawsets.KEEPGEO = True
    if '--minrep' in av:
        i = av.index('--minrep'); drawsets.MINREP = int(av[i + 1]); del av[i:i + 2]
    if '--combos' in av:
        av.remove('--combos'); drawsets.DROP = ('n', 'idx')   # compare shader / texture / state combinations, not vertex counts
    pos = []
    i = 0
    while i < len(av):
        if av[i] in ('--skip', '--top', '--only'):
            opt[av[i][2:]] = type(opt[av[i][2:]] or 0)(av[i + 1]) if av[i] != '--only' else av[i + 1]
            i += 2
        else:
            pos.append(av[i]); i += 1
    da, la, db, lb = pos
    sa, sb = segments(la), segments(lb)
    # last frame of the logs' dumps
    def last(d):
        return max(int(f[1:7]) for f in os.listdir(d) if re.match(r'f\d+\.txt', f))
    ea, eb = last(da), last(db)
    segs_a = [(f, sa[i + 1][0] - 1 if i + 1 < len(sa) else ea, n) for i, (f, n) in enumerate(sa)]
    segs_b = [(f, sb[i + 1][0] - 1 if i + 1 < len(sb) else eb, n) for i, (f, n) in enumerate(sb)]
    k = opt['skip']
    import difflib
    sm = difflib.SequenceMatcher(None, [n for _, _, n in segs_a], [n for _, _, n in segs_b], autojunk=False)
    pairs = []
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == 'equal':
            pairs += [(segs_a[i1 + t], segs_b[j1 + t]) for t in range(i2 - i1)]
        else:
            pairs += [(segs_a[i], None) for i in range(i1, i2)] + [(None, segs_b[j]) for j in range(j1, j2)]
    for pa, pb in pairs:
        if pa is None:
            print('%-28s only in B (frames %d-%d)' % (pb[2], pb[0], pb[1])); continue
        a0, a1, na = pa
        if pb is None:
            print('%-28s only in A (frames %d-%d)' % (na, a0, a1)); continue
        b0, b1, nb = pb
        if opt['only'] and opt['only'] != na: continue
        if a1 - a0 < 2 * k + 2 or b1 - b0 < 2 * k + 2:
            a0_, a1_, b0_, b1_ = a0, a1, b0, b1
        else:
            a0_, a1_, b0_, b1_ = a0 + k, a1 - k, b0 + k, b1 - k
        ca, fa = drawsets.sigs(da, a0_, a1_)
        cb, fb = drawsets.sigs(db, b0_, b1_)
        only_a = [(c, s) for s, c in ca.items() if s not in cb and c >= drawsets.MINREP]
        only_b = [(c, s) for s, c in cb.items() if s not in ca and c >= drawsets.MINREP]
        both = [s for s in ca if s in cb]
        odd = [(abs(ca[x] / max(fa, 1) - cb[x] / max(fb, 1)), x) for x in both
               if abs(ca[x] / max(fa, 1) - cb[x] / max(fb, 1)) > 0.5 * max(ca[x] / max(fa, 1), cb[x] / max(fb, 1)) and abs(ca[x] / max(fa, 1) - cb[x] / max(fb, 1)) > 0.2]
        print('%-28s A %5d-%-5d (%4d fr) B %5d-%-5d (%4d fr): distinct %4d / %4d, shared %4d, only A %3d (%5d draw-fr), only B %3d (%5d), rates differ %d' % (
            na, a0_, a1_, fa, b0_, b1_, fb, len(ca), len(cb), len(both), len(only_a), sum(c for c, _ in only_a), len(only_b), sum(c for c, _ in only_b), len(odd)))
        for d_, x in sorted(odd, reverse=True)[:opt['top']]:
            print('     rate A %.2f B %.2f per frame  %s' % (ca[x] / max(fa, 1), cb[x] / max(fb, 1), x[:200]))
        for tag, lst in (('A', sorted(only_a, reverse=True)), ('B', sorted(only_b, reverse=True))):
            for c, s in lst[:opt['top']]:
                print('     only %s x%-4d %s' % (tag, c, s[:230]))


if __name__ == '__main__':
    main()
