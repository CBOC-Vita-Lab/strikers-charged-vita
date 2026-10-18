#!/usr/bin/env python3
"""W5-SEED: what a set of runs would add to a catalogue, as a SMALL list for a delta compile.

  newkeys.py <catalogue.shl> <out.shl> run.shl...

out.shl = every program of the runs that uses a shader (vs or fs) missing from the catalogue, with its two shaders and its variants. A Vita3K run warming this list
compiles only the new shaders (the profile's cache answers the known ones). Prints how many shaders / programs / variants are new, and exits 0 either way
(out.shl is not written when nothing is new).
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shl


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    base = shl.read(sys.argv[1])
    D = shl.SList(); D.vsSize, D.fsSize, D.gen = base.vsSize, base.fsSize, base.gen
    newvs = set(); newfs = set()
    for p in sys.argv[3:]:
        L = shl.read(p)
        bad = set()
        for v, f in L.prog:
            if not base.has_vs(v) or not base.has_fs(f):
                bad.add((v, f)); D.add_vs(v); D.add_fs(f); D.add_prog(v, f)
                if not base.has_vs(v): newvs.add(v)
                if not base.has_fs(f): newfs.add(f)
        for t in L.var:
            if (t[0], t[1]) in bad:
                D.add_var(*t)
    D.dups = 0
    print('new shaders: %d vertex, %d fragment; programs %d, variants %d' % (len(newvs), len(newfs), len(D.prog), len(D.var)))
    if newvs or newfs:
        shl.write(D, sys.argv[2])
        print('written', sys.argv[2])
    else:
        print('nothing new; %s not written' % sys.argv[2])


if __name__ == '__main__':
    main()
