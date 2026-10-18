#!/usr/bin/env python3
"""ubsets.py: for the draws two runs have in common (same signature: shader, layout, textures, state), do they also use the same values of a uniform
block? Block 5 is the TEV block (constant colours, alpha reference, fog, texture sizes), 4 the lights, 1 the model-view matrices, 2 texture matrices.
Time-independent blocks (5, often 4) should coincide as SETS of values in the same scene; animated ones (1, 2) do not.
  ubsets.py A_DIR RANGE_A B_DIR RANGE_B [--block 5] [--top K]"""
import os, re, sys, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drawsets

def collect(d, a, b, blk):
    sig = collections.defaultdict(set)
    for n in range(a, b + 1):
        p = os.path.join(d, 'f%06d.txt' % n)
        if not os.path.exists(p): continue
        for line in open(p):
            if not line.startswith('D '): continue
            parts = line.rstrip('\n').split(' ')[2:]
            s = ' '.join(x for x in parts if not drawsets.SOFT.match(x) and x.split('=')[0] not in drawsets.DROP)
            for a_, b_ in drawsets.SUBS: s = s.replace(a_, b_)
            m = re.search(r'\bub%d=(\w+)' % blk, line)
            sig[s].add(m.group(1) if m else '-')
    return sig

def main():
    av = sys.argv[1:]
    blk = 5; top = 6
    while '--block' in av:
        i = av.index('--block'); blk = int(av[i + 1]); del av[i:i + 2]
    while '--top' in av:
        i = av.index('--top'); top = int(av[i + 1]); del av[i:i + 2]
    drawsets.DROP = ('n', 'idx')
    da, ra, db, rb = av[:4]
    a0, a1 = map(int, ra.split('-')); b0, b1 = map(int, rb.split('-'))
    A = collect(da, a0, a1, blk); B = collect(db, b0, b1, blk)
    shared = [s for s in A if s in B]
    same = sum(1 for s in shared if A[s] & B[s])
    disjoint = [s for s in shared if not (A[s] & B[s])]
    print('block %d: %d shared signatures, %d use a common value, %d have disjoint value sets' % (blk, len(shared), same, len(disjoint)))
    for s in disjoint[:top]:
        print('  A %s B %s  %s' % (sorted(A[s])[:3], sorted(B[s])[:3], s[:150]))

if __name__ == '__main__':
    main()
