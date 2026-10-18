#!/usr/bin/env python3
"""extract.py <hex-addr>... : write the translated C++ of those guest functions (from split/gen-trunk base_common shards) to stdout."""
import pickle, sys, re
idx = pickle.load(open(__import__('os').path.join(__import__('os').path.dirname(__import__('os').path.abspath(__file__)), 'index.pkl'), 'rb'))
cache = {}
out = []
for a in sys.argv[1:]:
    addr = int(a, 16)
    f, s, e = idx[addr]
    if f not in cache: cache[f] = open(f, errors='replace').read().split('\n')
    lines = cache[f][s:e]
    # drop the trailing "// RECOMP_GUEST_ABI" comment line that belongs to the next function
    while lines and (lines[-1].startswith('// RECOMP_GUEST_ABI') or not lines[-1].strip()): lines.pop()
    out.append('\n'.join(lines))
print('\n'.join(out))
