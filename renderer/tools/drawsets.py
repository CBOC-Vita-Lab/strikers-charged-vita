#!/usr/bin/env python3
"""drawsets.py: compare, per scene, WHICH draws two runs make (native game against the recompiled build), independent of timing.

  drawsets.py A_DIR RANGE_A B_DIR RANGE_B [--name N] [--top K]

RANGE = first-last frame (e.g. 1018-1154). A draw's signature is everything of its record line except the content hashes of the
uniforms and vertices (ubN, geo): shaders, layout, vertex / index counts, texture contents, wraps, filters, render state. For every
signature the report counts the frames in which it occurs in each run; signatures that occur in one run only are listed. A signature
that exists in both but with very different frame counts is listed too (a draw that is missing for part of the scene).
Texture ids are content hashes (drawlog), so equal pictures give equal signatures. Reads the logs of tools/drawdiff.py (non-FULL ones are fine).
"""
import os, re, sys, collections

SOFT = re.compile(r'^(geo|ub\d|tl\d)=')
SOFT_NOGEO = re.compile(r'^(ub\d|tl\d)=')   # (tlN: authored mip levels of a texture: a back end / recompiled-build matter, listed by drawdiff)


DROP = ()   # field names left out of the signature (set by --combos: ('n', 'idx'))
SUBS = []   # --sub A=B: replace A by B in every signature (known, understood differences)
KEEPGEO = False   # --geo: the vertex content hash is part of the signature (logs made with MSC_VGX_HASH=2 give one for cached display lists too)
MINREP = 1   # --minrep K: list only signatures that occur at least K times in their run (static geometry repeats, a moving mesh does not)


def sigs(d, a, b):
    cnt = collections.Counter()
    frames = 0
    for n in range(a, b + 1):
        p = os.path.join(d, 'f%06d.txt' % n)
        if not os.path.exists(p): continue
        frames += 1
        seen = set()
        for line in open(p):
            if line.startswith('CT ') or line.startswith('CD '):
                # EFB copies: what is copied (rectangle, size, depth, clear, colour matrix); skip / pass / fill are decisions of vgx's analysis
                parts = [x for x in line.rstrip('\n').split(' ') if not re.match(r'(skip|pass|fill)=', x)]
                cnt['COPY ' + ' '.join(parts)] += 1
                continue
            if not line.startswith('D '): continue
            parts = line.rstrip('\n').split(' ')[2:]
            s = ' '.join(x for x in parts if not (SOFT_NOGEO if KEEPGEO else SOFT).match(x) and x.split('=')[0] not in DROP)
            # the draw's index within the frame is not part of it; count the multiplicity per frame instead
            for a_, b_ in SUBS: s = s.replace(a_, b_)
            cnt[s] += 1
        # (multiplicities are summed over frames)
    return cnt, frames


def main():
    av = sys.argv[1:]
    top = 12
    name = ''
    global DROP, KEEPGEO, MINREP
    if '--geo' in av:
        av.remove('--geo'); KEEPGEO = True
    while '--sub' in av:
        i = av.index('--sub'); SUBS.append(tuple(av[i + 1].split('=', 1))); del av[i:i + 2]
    if '--minrep' in av:
        i = av.index('--minrep'); MINREP = int(av[i + 1]); del av[i:i + 2]
    if '--combos' in av:
        av.remove('--combos'); DROP = ('n', 'idx')
    while '--top' in av:
        i = av.index('--top'); top = int(av[i + 1]); del av[i:i + 2]
    while '--name' in av:
        i = av.index('--name'); name = av[i + 1]; del av[i:i + 2]
    da, ra, db, rb = av[:4]
    a0, a1 = map(int, ra.split('-')); b0, b1 = map(int, rb.split('-'))
    ca, fa = sigs(da, a0, a1)
    cb, fb = sigs(db, b0, b1)
    only_a = [(c, s) for s, c in ca.items() if s not in cb and c >= MINREP]
    only_b = [(c, s) for s, c in cb.items() if s not in ca and c >= MINREP]
    both = [s for s in ca if s in cb]
    # per-frame rate comparison for shared signatures (a draw drawn in only half the frames of one run)
    odd = []
    for s in both:
        ra_, rb_ = ca[s] / max(fa, 1), cb[s] / max(fb, 1)
        if abs(ra_ - rb_) > 0.5 * max(ra_, rb_) and abs(ra_ - rb_) > 0.2:
            odd.append((abs(ra_ - rb_), ra_, rb_, s))
    print('%s frames %d-%d (%d) vs %d-%d (%d): %d / %d distinct draws, %d shared, %d only in A (%d draw-frames), %d only in B (%d), %d shared with different rates' % (
        name, a0, a1, fa, b0, b1, fb, len(ca), len(cb), len(both), len(only_a), sum(c for c, _ in only_a), len(only_b), sum(c for c, _ in only_b), len(odd)))
    for tag, lst in (('A only', sorted(only_a, reverse=True)), ('B only', sorted(only_b, reverse=True))):
        for c, s in lst[:top]:
            print('  %s x%-5d %s' % (tag, c, s[:260]))
    for d_, ra_, rb_, s in sorted(odd, reverse=True)[:top]:
        print('  rate A %.2f/frame B %.2f/frame  %s' % (ra_, rb_, s[:240]))


if __name__ == '__main__':
    main()
