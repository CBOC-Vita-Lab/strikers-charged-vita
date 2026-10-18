#!/usr/bin/env python3
"""Lists every GX / VI / TPL (and GD) function the game source calls.

  gx_calls.py [--np DIR] [--rvl DIR] [--out calls.tsv]

Scans <np>/src and <np>/include for identifiers of the form GX*( VI*( TPL*( GD*( (and __GX*), keeps those the
decompilation's public SDK headers declare, and writes name, number of call sites, declaring header, first call sites.
Names that are not SDK functions (the game's own GX*MaterialProgram classes, GXProject which the game declares itself)
are listed on stderr. WGPIPE (the gather pipe) is counted separately.
"""
import argparse, os, re, sys, collections
ap = argparse.ArgumentParser()
ap.add_argument('--np', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
ap.add_argument('--rvl', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'libs', 'RVL_SDK', 'include', 'revolution'))
ap.add_argument('--out', default='-')
a = ap.parse_args()
H = a.rvl
decl = {}
for root, _, fs in os.walk(H):
    for f in fs:
        if not f.endswith('.h'): continue
        p = os.path.join(root, f)
        t = open(p, errors='replace').read()
        for m in re.finditer(r'\b((?:__)?(?:GX|GD|VI|TPL)[A-Za-z0-9_]*)\s*\(', t):
            decl.setdefault(m.group(1), set()).add(os.path.relpath(p, H))
use = collections.defaultdict(list)
wg = 0
for base in ('src', 'include'):
    for root, _, fs in os.walk(os.path.join(a.np, base)):
        for f in fs:
            if not f.endswith(('.c', '.cpp', '.h', '.hpp', '.inl', '.inc')): continue
            p = os.path.join(root, f)
            for i, l in enumerate(open(p, errors='replace'), 1):
                wg += len(re.findall(r'\bWGPIPE\b', l))
                for m in re.finditer(r'\b((?:__)?(?:GX|GD|VI|TPL)[A-Za-z0-9_]*)\s*\(', l):
                    use[m.group(1)].append((os.path.relpath(p, a.np), i))
out = sys.stdout if a.out == '-' else open(a.out, 'w')
n = 0
for name in sorted(use):
    if name in decl:
        out.write('%s\t%d\t%s\t%s\n' % (name, len(use[name]), ','.join(sorted(decl[name])), ';'.join('%s:%d' % x for x in use[name][:3])))
        n += 1
    elif not re.search(r'(MaterialProgram|Parameters)', name):
        sys.stderr.write('not declared by the SDK headers (game-declared or its own): %s (%d) %s:%d\n' % (name, len(use[name]), *use[name][0]))
sys.stderr.write('%d SDK functions called, WGPIPE mentioned %d times\n' % (n, wg))
