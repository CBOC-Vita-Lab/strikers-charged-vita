#!/usr/bin/env python3
"""unmapped.py <outdir> : retail mutable data symbols that the translated code references (r13/r2-relative and lis/addi absolute addresses) but that have no native counterpart in dtl_tables.inc"""
import re, sys, bisect
out = sys.argv[1]
gen = open(out + '/dtl_gen.inc', errors='replace').read()
tab = open(out + '/dtl_tables.inc', errors='replace').read()
mapped = set(int(m.group(1), 16) for m in re.finditer(r'\{ 0x([0-9A-F]{8})u, \d+, "[^"]*", \d \}', tab))
mapped |= set(int(m.group(1), 16) for m in re.finditer(r'\{ 0x([0-9A-F]{8})u, \d+, "[^"]+" \}', tab))
syms = []
for l in open('/home/claude/research/decomp/mscharged-decomp/config/R4QE01/symbols.txt'):
    m = re.match(r'^(\S+) = \.(data|bss|sdata|sbss|sbss2):0x([0-9A-Fa-f]+); // (.*)$', l.strip())
    if m:
        sz = re.search(r'size:0x([0-9A-Fa-f]+)', m.group(4)); syms.append((int(m.group(3), 16), int(sz.group(1), 16) if sz else 0, m.group(1)))
syms.sort(); starts = [s[0] for s in syms]
def find(a):
    i = bisect.bisect_right(starts, a) - 1
    if i < 0: return None
    s = syms[i]
    return s if a < s[0] + max(s[1], 1) else None
addrs = collections = {}
import collections
cnt = collections.Counter()
for m in re.finditer(r'\(r13 \+ (-?\d+)\)', gen): cnt[(0x806E33C0 + int(m.group(1))) & 0xFFFFFFFF] += 1
for m in re.finditer(r'\(r2 \+ (-?\d+)\)', gen): cnt[(0x806EB060 + int(m.group(1))) & 0xFFFFFFFF] += 1
# lis-style: rN = 0x80xx0000u; ... (rN + off)
for m in re.finditer(r'(r\d+) = (0x80[0-9A-F]{2}0000)u;\s*\n(?:[^\n]*\n){0,6}?[^\n]*\(\1 \+ (-?\d+)\)', gen):
    cnt[(int(m.group(2), 16) + int(m.group(3))) & 0xFFFFFFFF] += 1
unm = collections.Counter(); seen = {}
for a, n in cnt.items():
    s = find(a)
    if s and s[0] not in mapped:
        unm[s[2]] += n; seen[s[2]] = a
for name, n in unm.most_common():
    print('%-60s %08x  refs %d' % (name, seen[name], n))
print(len(unm), 'unmapped mutable symbols referenced;', len([1 for a in cnt if find(a) and find(a)[0] in mapped]), 'referenced addresses are mapped')
