#!/usr/bin/env python3
"""Generate platform/stubs/sdk_stubs.c (link stubs for every unresolved SDK symbol) and
docs/sdk-symbols.tsv (module, kind, return type, source call count) from a link attempt.

usage: tools/gen_stubs.py <link.err> <relocs.txt> <out_stubs.c> <out_symbols.tsv> [overrides.txt]
  link.err     stderr of a link of every game object (ld: "undefined reference to `sym'")
  relocs.txt   `readelf -rW` of the combined relocatable object (to tell function from data references)
  overrides    optional lines "sym data|func|skip [ctype]" (tools/stub_overrides.txt)
Symbols are looked up in libs/RVL_SDK/include (and MSL/Runtime) to get the declared return type and module.
"""
import re, sys, os, collections, subprocess

link_err, relocs, out_c, out_tsv = sys.argv[1:5]
overrides = {}
if len(sys.argv) > 5 and os.path.exists(sys.argv[5]):
    for line in open(sys.argv[5]):
        line = line.split('#')[0].split()
        if len(line) >= 2:
            overrides[line[0]] = (line[1], ' '.join(line[2:]))

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
syms = collections.OrderedDict()
for m in re.finditer(r"undefined reference to [`'\"]?([^`'\"\s]+)[`'\"]?", open(link_err, errors='replace').read()):
    syms.setdefault(m.group(1), 0)
    syms[m.group(1)] += 1

# function vs data from relocation types
CALL = re.compile(r'(PLT32|THM_CALL|CALL|JUMP24|THM_JUMP|THM_JUMP24|THM_JUMP19|PC24|PLT)')
callsyms, datasyms = set(), set()
for line in open(relocs, errors='replace'):
    f = line.split()
    if len(f) >= 5 and f[2].startswith('R_'):
        name = f[4].split('@')[0]
        (callsyms if CALL.search(f[2]) else datasyms).add(name)

# header scan: return type and module of each symbol
inc_roots = [os.path.join(ROOT, 'libs/RVL_SDK/include'), os.path.join(ROOT, 'libs/MSL_C/include'),
             os.path.join(ROOT, 'libs/Runtime/include'), os.path.join(ROOT, 'include')]
hdrs = []
for r in inc_roots:
    for d, _, fs in os.walk(r):
        for fn in fs:
            if fn.endswith(('.h', '.inl')):
                hdrs.append(os.path.join(d, fn))
hdr_text = {h: open(h, errors='replace').read().replace('extern "C"', '') for h in hdrs}

def find_decl(sym):
    pat = re.compile(r'(?m)^[ \t]*((?:extern[ \t]+)?(?:static[ \t]+)?(?:inline[ \t]+)?(?:const[ \t]+)?(?:volatile[ \t]+)?[A-Za-z_][\w \t\*]*?[ \t\*])' + re.escape(sym) + r'[ \t]*\(')
    for h, t in hdr_text.items():
        m = pat.search(t)
        if m:
            rt = m.group(1)
            rt = re.sub(r'\b(extern|static|inline|DECL_WEAK|ATTRIBUTE_ALIGN\(\d+\))\b', '', rt).strip()
            return h, rt
    return None, None

def module_of(h):
    if not h: return 'unknown'
    rel = os.path.relpath(h, ROOT)
    if rel.startswith('libs/RVL_SDK/include/dwc/'): return 'dwc'
    if rel.startswith('libs/RVL_SDK/include/RVLFaceLib'): return 'rfl'
    m = re.match(r'libs/RVL_SDK/include/(?:revolution|nitro)/([^/.]+)', rel)
    if m: return m.group(1)
    m = re.match(r'libs/RVL_SDK/include/([^/]+)', rel)
    if m: return m.group(1)
    if rel.startswith('libs/MSL_C'): return 'MSL_C'
    if rel.startswith('libs/Runtime'): return 'Runtime'
    return 'game'

# source call counts (src + include, word match)
def count_uses(sym):
    try:
        out = subprocess.run(['grep', '-rwc', '--include=*.cpp', '--include=*.c', '--include=*.h', '--include=*.inl', sym,
                              os.path.join(ROOT, 'src'), os.path.join(ROOT, 'include')],
                             capture_output=True, text=True).stdout
    except Exception:
        return 0, 0
    n = f = 0
    for l in out.splitlines():
        p, c = l.rsplit(':', 1)
        if int(c):
            n += int(c); f += 1
    return n, f

def demangle(names):
    try:
        out = subprocess.run(['c++filt'], input='\n'.join(names) + '\n', capture_output=True, text=True).stdout.split('\n')
        return dict(zip(names, out))
    except Exception:
        return {n: n for n in names}
dem = demangle([s for s in syms if s.startswith('_Z')])

rows = []
for s in syms:
    ov = overrides.get(s)
    if ov and ov[0] == 'skip':
        continue
    if s.startswith('_Z'):
        d = dem.get(s, s)
        # functions (virtual ones are only referenced from vtables, so relocation types cannot tell) have a
        # parameter list in their demangled form; vtables get their own kind
        kind = 'cxxvtable' if s.startswith('_ZTV') else ('cxx' if '(' in d else 'cxxdata')
        rows.append((s, kind, 'game', dem.get(s, s), 0, 0))   # use counts of mangled names are not meaningful
        continue
    h, rt = find_decl(s)
    kind = ov[0] if ov else ('func' if (s in callsyms or h) or s not in datasyms else 'data')
    if ov and ov[1]:
        rt = ov[1]
    rows.append((s, kind, module_of(h), rt or '', *count_uses(s)))

def ctype(rt):
    """C type of the stub's return value from the declared return type text (typedef names are not visible
    to the stub TU, so only the register class matters: pointer, float, double, 64-bit or word)."""
    rt = (rt or '').strip()
    if rt == 'void': return 'void'
    if '*' in rt: return 'void*'
    if re.search(r'\b(f32|float)\b', rt): return 'float'
    if re.search(r'\b(f64|double)\b', rt): return 'double'
    if re.search(r'\b(s64|u64|OSTime|long long|__int64)\b', rt): return 'long long'
    return 'int'

with open(out_c, 'w') as f:
    f.write('/* GENERATED by tools/gen_stubs.py from a link of every game object. DO NOT EDIT BY HAND\n'
            ' * (put overrides in tools/stub_overrides.txt and regenerate: make TARGET=host32 stubs).\n'
            ' * Each stub reports its name once on stderr and returns 0; with NP_STUB_ABORT=1 in the\n'
            ' * environment the first call aborts. NPS/NPR/NPA replace stubs with real implementations\n'
            ' * (a real definition anywhere in the link simply removes the symbol from this list). */\n'
            '#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n\n'
            '#include "np_stub_support.h"\n\n')
    for i, (s, kind, mod, rt, n, nf) in enumerate(rows):
        if kind == 'cxx':
            # C++ function without a definition anywhere (a gap in the decomp or a missing out-of-line inline)
            f.write('NP_WEAK int np_stubx_%d() __asm__("%s");\n' % (i, s))
            f.write('NP_WEAK int np_stubx_%d() { NP_STUB_HIT("%s"); return 0; }\n' % (i, rt.replace('\\', '\\\\').replace('"', '\\"')))
        elif kind == 'cxxvtable':
            # a vtable whose key function is a gap in the decomp: 64 slots that report and abort when a virtual
            # is called (vptr points at slot 2 of the Itanium layout: offset-to-top, typeinfo, functions...)
            f.write('NP_WEAK void *np_stubv_%d[66] __asm__("%s") = { 0, 0, ' % (i, s) + ', '.join(['np_stub_vcall'] * 64) + ' }; /* %s */\n' % rt)
        elif kind == 'cxxdata':
            f.write('NP_WEAK char np_stubd_%d[4096] __asm__("%s"); /* %s */\n' % (i, s, rt))
        elif kind == 'data':
            f.write('NP_WEAK char %s[4096]; /* data symbol referenced by the game (%s); zero bytes */\n' % (s, mod))
        else:
            ct = ctype(rt)
            if ct == 'void':
                f.write('NP_WEAK void %s() { NP_STUB_HIT("%s"); }\n' % (s, s))
            else:
                f.write('NP_WEAK %s %s() { NP_STUB_HIT("%s"); return (%s)0; }\n' % (ct, s, s, ct))
with open(out_tsv, 'w') as f:
    f.write('symbol\tkind\tmodule\treturn_type\tsource_uses\tsource_files\n')
    for r in sorted(rows, key=lambda r: (r[2], -r[4], r[0])):
        f.write('\t'.join(str(x) for x in r) + '\n')
print('%d unresolved symbols: %d C functions, %d C data, %d C++ functions, %d C++ data (vtables...)' % (len(rows), sum(1 for r in rows if r[1]=='func'), sum(1 for r in rows if r[1]=='data'), sum(1 for r in rows if r[1]=='cxx'), sum(1 for r in rows if r[1] in ('cxxdata','cxxvtable'))))
