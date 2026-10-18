#!/usr/bin/env python3
"""Compare the Wii layouts documented in the decomp headers (`/* 0xOFF */ type name;` and
`}; // total size: 0xNN`) with the native record layouts clang dumps for a target.

usage: tools/layout_census.py <layout dump from clang -Xclang -fdump-record-layouts> [--list]
Dump one with:  clang++ <target flags> -fsyntax-only -Xclang -fdump-record-layouts all_headers.cpp
(see docs/build.md, "layout census"). Prints counts and, with --list, every mismatch."""
import re, sys, os, collections
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if sys.argv[1] == '--gen-tu':     # write a TU that includes every game header and takes sizeof of every class
    out = sys.argv[2]; names = set(); hdrs = []
    for d, _, fs in os.walk(os.path.join(ROOT, 'include')):
        for fn in sorted(fs):
            if not fn.endswith('.h'): continue
            p = os.path.join(d, fn); rel = os.path.relpath(p, os.path.join(ROOT, 'include'))
            if rel.split('/')[0] in ('Game', 'NL', 'ode'): hdrs.append(rel)
            L = open(p, errors='replace').read().split('\n')
            for i, l in enumerate(L):
                m = re.match(r'^(?:class|struct|union)\s+([A-Za-z_]\w*)\b(?!\s*;)', l)
                if m and not (i and L[i-1].strip().startswith('template')) and ';' not in l.split('{')[0]:
                    names.add(m.group(1))
    with open(out, 'w') as f:
        f.write('#include <revolution/types.h>\n#include "types.h"\n')
        for h in sorted(hdrs): f.write('#include "%s"\n' % h)
        for n in sorted(names): f.write('static const unsigned np_sz_%s = sizeof(%s);\n' % (n, n))
    sys.exit(0)
dump, listing = sys.argv[1], '--list' in sys.argv

# ---- native layouts
recs = {}          # name -> (size, {field: offset(bytes)})
cur = None
for line in open(dump, errors='replace'):
    m = re.match(r'^\s*(\d+)(?::\d+-\d+)?\s*\|\s{0,1}(class|struct|union) ([A-Za-z_]\w*)\s*$', line)
    if m and m.group(1) == '0' and not line.startswith('           |') and re.match(r'^\s*0 \| (class|struct|union) ', line):
        cur = m.group(3); recs.setdefault(cur, [None, {}, 0]); 
        if recs[cur][0] is not None:
            recs[cur][2] += 1  # duplicate name
        continue
    if cur is None: continue
    m = re.match(r'^\s*(\d+)(?::\d+-\d+)?\s*\|   (?! )(.*?)\s*$', line)   # direct member (indent 2)
    if m and not line.lstrip().startswith('|'):
        txt = m.group(2)
        fm = re.search(r'([A-Za-z_]\w*)(\[[^\]]*\])*$', txt)
        if fm and 'base)' not in txt:
            recs[cur][1].setdefault(fm.group(1), int(m.group(1)))
        continue
    m = re.search(r'sizeof=(\d+)', line)
    if m and recs[cur][0] is None:
        recs[cur][0] = int(m.group(1))

# ---- documented layouts
doc = {}           # name -> (total or None, {field: off}, header)
for d, _, fs in os.walk(os.path.join(ROOT, 'include')):
    for fn in fs:
        if not fn.endswith('.h'): continue
        p = os.path.join(d, fn)
        lines = open(p, errors='replace').read().split('\n')
        i = 0
        while i < len(lines):
            m = re.match(r'^\s*(?:class|struct|union)\s+([A-Za-z_]\w*)\b(?!\s*;)', lines[i])
            if not m or (i and lines[i-1].strip().startswith('template')) or ';' in lines[i].split('{')[0]:
                i += 1; continue
            name = m.group(1); depth = 0; started = False; fields = {}; total = None; j = i
            while j < len(lines):
                L = lines[j]
                code = re.sub(r'/\*.*?\*/|//.*', '', L)
                if started and depth == 1:
                    fm = re.match(r'^\s*/\*\s*0x([0-9A-Fa-f]+)\s*\*/\s*(.*?);', L)
                    if fm:
                        nm = re.search(r'([A-Za-z_]\w*)\s*(\[[^\]]*\])*\s*(?::\s*\d+\s*)?$', fm.group(2))
                        if nm and '(' not in fm.group(2):
                            fields.setdefault(nm.group(1), int(fm.group(1), 16))
                depth += code.count('{') - code.count('}')
                if '{' in code: started = True
                if started and depth <= 0:
                    tm = re.search(r'total size:\s*0x([0-9A-Fa-f]+)', L) or (re.search(r'total size:\s*0x([0-9A-Fa-f]+)', lines[j+1]) if j + 1 < len(lines) else None)
                    if tm: total = int(tm.group(1), 16)
                    break
                j += 1
            if (fields or total is not None):
                if name in doc: doc[name] = None   # ambiguous
                else: doc[name] = (total, fields, os.path.relpath(p, ROOT))
            i = max(j, i) + 1

nsz = nsz_bad = nfld = nfld_bad = 0
bad_size, bad_fields = [], []
for name, v in sorted(doc.items()):
    if v is None or name not in recs or recs[name][2]: continue
    total, fields, hdr = v
    size, nf, _ = recs[name]
    if total is not None and size is not None:
        nsz += 1
        if total != size:
            nsz_bad += 1; bad_size.append((name, hdr, total, size))
    for f, off in fields.items():
        if f in nf:
            nfld += 1
            if nf[f] != off:
                nfld_bad += 1; bad_fields.append((name, hdr, f, off, nf[f]))
print('classes with documented size compared: %d, size mismatches: %d' % (nsz, nsz_bad))
print('documented field offsets compared: %d, mismatches: %d (in %d classes)' % (nfld, nfld_bad, len({b[0] for b in bad_fields})))
if listing:
    for b in bad_size: print('SIZE  %-40s %-45s wii 0x%X native 0x%X' % b)
    first = {}
    for b in bad_fields: first.setdefault(b[0], b)
    for n, b in first.items(): print('FIELD %-40s %-45s first at %s: wii 0x%X native 0x%X' % (b[0], b[1], b[2], b[3], b[4]))
