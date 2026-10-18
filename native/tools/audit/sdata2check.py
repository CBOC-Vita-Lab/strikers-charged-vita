#!/usr/bin/env python3
"""sdata2check.py <source.cpp> <start> <end> [doublewords...]: floats/doubles of the retail .sdata2 range of the TU (MWCC pools one copy per distinct constant)
against the numeric literals of the source.  Prints retail constants never seen in the source and source literals not in the retail pool."""
import re, struct, sys
img = open('/home/claude/msc/npw/NPG-tools/dol.flat', 'rb').read(); B = 0x80004000
src = open(sys.argv[1]).read()
a, b = int(sys.argv[2], 16), int(sys.argv[3], 16)
# strip comments, strings, data label initialisers handled separately (they live in .sdata)
src = re.sub(r'//[^\n]*', '', src); src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
lits = {}
for m in re.finditer(r'(?<![\w.])(-?\d+\.\d*(?:[eE][-+]?\d+)?|-?\.\d+(?:[eE][-+]?\d+)?|-?\d+[eE][-+]?\d+)([fF]?)(?![\w.])', src):
    v = float(m.group(1)); isf = m.group(2) != ''
    lits.setdefault((struct.unpack('>f', struct.pack('>f', v))[0] if isf else v, isf), 0); lits[(struct.unpack('>f', struct.pack('>f', v))[0] if isf else v, isf)] += 1
words = []
addr = a
retail = []
while addr < b:
    w = struct.unpack('>I', img[addr-B:addr-B+4])[0]; f = struct.unpack('>f', img[addr-B:addr-B+4])[0]
    retail.append((addr, w, f)); addr += 4
srcvals = set(v for (v, _) in lits)
print('retail .sdata2 words: %d, distinct source numeric literals: %d' % (len(retail), len(lits)))
missing = []
for addr, w, f in retail:
    ok = any(abs(f - v) <= 1e-6 * max(1.0, abs(f)) for v in srcvals) or w == 0
    d = struct.unpack('>d', img[addr-B:addr-B+8])[0] if addr + 8 <= b else None
    okd = d is not None and any(abs(d - v) <= 1e-9 * max(1.0, abs(d)) for v in srcvals)
    if not ok and not okd: missing.append((addr, w, f, d))
print('retail words with no equal source literal (could be double halves / int-to-float magic / inlined from headers):')
for addr, w, f, d in missing: print('  %08x  %08x  float=%.9g' % (addr, w, f) + ('  double=%.12g' % d if d is not None else ''))
retvals = [f for _, _, f in retail]
retd = [struct.unpack('>d', img[a2-B:a2-B+8])[0] for a2 in range(a, b-4, 4)]
extra = [(v, isf) for (v, isf) in lits if not any(abs(v - r) <= 1e-6 * max(1.0, abs(v)) for r in retvals) and not any(abs(v - r) <= 1e-9 * max(1.0, abs(v)) for r in retd)]
print('source literals not in the retail pool (zero/one/etc. may be encoded as immediates or live in .sdata):')
print('  ', sorted(extra))
