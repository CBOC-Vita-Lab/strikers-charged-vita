#!/usr/bin/env python3
"""gen_bf.py <exe> <outdir> : bit-field tables for dtlive (dtl_bf.inc).

MWCC allocates bit-fields from the most significant bit of the storage unit (first declared field = top), clang from the least significant bit. The recompiler's translation of retail code reads and
writes the native objects with the retail positions, so for every polymorphic class that has bit-fields the harness needs to know where the fields are:  clang's own layout dump
(`-Xclang -fdump-record-layouts`, one -O0 -emit-llvm pass over a few TUs, about 0.5 s each) gives the byte offset, first and last bit of each field (nested members and bases expanded, absolute offsets),
the declared type gives the unit width W (8/16/32).  A field of n bits at bit lo of its W-bit unit is at bit  W - lo - n  in the retail layout.
Only classes with a vtable are tabulated (the harness finds the objects by their vtable pointer); the symbol `_ZTV<name>` of the native executable gives the address.
Output: static const BfField / BfUnit / BfClass tables."""
import re, subprocess, sys, os, collections

NP = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
TUS = ['src/Game/Goalie.cpp', 'src/Game/Character.cpp', 'src/Game/AI/Fielder.cpp', 'src/Game/Ball.cpp', 'src/Game/AI/GoalieActions.cpp', 'src/Game/PoseAccumulator.cpp',
       'src/Game/AI/GoalieSave.cpp', 'src/Game/Player.cpp', 'src/Game/Team.cpp']
FLAGS = ('-m32 -msse2 -mfpmath=sse -isystem /usr/include/x86_64-linux-gnu -DNP_TARGET_HOST32=1 -DNP_NATIVE=1 -DNP_DISC_HOST_ORDER=1 -DBUILD_VERSION=0 -DVERSION_R4QE01 -DNDEBUG=1 -DdNODEBUG=1 -DdSINGLE=1 '
         '-Iplatform/gx/include -I. -Iinclude -Isrc/ode -Isrc/zlib -Ilibs/RVL_SDK/include -Iref/RVL_SDK -Ilibs/Runtime/include -idirafter libs/MSL_C/include -idirafter libs/MetroTRK/include '
         '-fno-strict-aliasing -fwrapv -fno-short-enums -fshort-wchar -fno-delete-null-pointer-checks -include np/compat.h -fno-math-errno -std=gnu++11 -fno-exceptions -fno-rtti -fno-strict-return '
         '-Iplatform/sdk/include -funsigned-char -w -O0 -S -emit-llvm -o /dev/null -Xclang -fdump-record-layouts').split()
WIDTH = {'u8': 8, '_Bool': 8, 'bool': 8, 'unsigned char': 8, 'char': 8, 'signed char': 8, 's8': 8,
         'u16': 16, 'unsigned short': 16, 'short': 16, 's16': 16}
BF = re.compile(r'^\s+(\d+):(\d+)-(\d+) \| (\s*)(.*?)\s*$')
ENT = re.compile(r'^\s+(\d+) \| (\s*)(.*)$')


def parse(path):
    recs = []; cur = None
    for l in open(path, errors='replace'):
        l = l.rstrip('\n')
        if l.startswith('*** Dumping AST Record Layout'):
            cur = []; recs.append(cur); continue
        if cur is None:
            continue
        if re.match(r'^\s+\d+(:\d+-\d+)? \|', l) or re.match(r'^\s+\| \[', l):
            cur.append(l)
    return recs


def main():
    exe, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    classes = collections.OrderedDict()
    for tu in TUS:
        p = os.path.join(NP, tu)
        if not os.path.exists(p):
            continue
        dump = os.path.join(outdir, 'lay_' + os.path.basename(tu) + '.txt')
        with open(dump, 'w') as f:
            subprocess.run(['clang++'] + FLAGS + [tu], cwd=NP, stdout=f, stderr=subprocess.DEVNULL)
        for r in parse(dump):
            if not r:
                continue
            m = ENT.match(r[0])
            if not m or m.group(1) != '0':
                continue
            head = m.group(3)
            mm = re.match(r'^(class|struct) ([A-Za-z_]\w*)$', head)       # plain names only (no templates / anonymous)
            if not mm:
                continue
            name = mm.group(2)
            if name in classes:
                continue
            body = r[1:]
            if not any('vtable pointer' in l for l in body[:16]):
                continue
            units = {}                                  # container start bit -> [W, [(lo,n)...]]
            for l in body:
                b = BF.match(l)
                if not b:
                    continue
                off, lo, hi = int(b.group(1)), int(b.group(2)), int(b.group(3))
                ty = b.group(5).rsplit(' ', 1)[0] if ' ' in b.group(5) else b.group(5)
                ty = re.sub(r'\bconst\b|\bvolatile\b', '', ty).strip()
                W = WIDTH.get(ty, 32)
                a0 = off * 8 + lo; a1 = off * 8 + hi
                c0 = (a0 // W) * W
                if a1 >= c0 + W:                        # straddles its unit: not handled
                    continue
                units.setdefault(c0, [W, []])[1].append((a0 - c0, a1 - a0 + 1))
                units[c0][0] = max(units[c0][0], W)
            if not units:
                continue
            # merge containers of different widths that overlap (a u8 unit inside a u32 unit): keep the wider one at its aligned start
            merged = {}
            for c0 in sorted(units):
                W, fl = units[c0]
                base = (c0 // W) * W
                if base in merged:
                    off2 = c0 - base
                    merged[base][1] += [(lo + off2, n) for lo, n in fl]
                else:
                    merged[base] = [W, list(fl)]
            classes[name] = merged
    # native vtable symbols
    nm = subprocess.run(['nm', '-C', '--defined-only', exe], capture_output=True, text=True).stdout
    vts = {}
    for l in nm.split('\n'):
        mm = re.match(r'^[0-9a-f]+ [dDrRvVwW] vtable for (\w+)$', l)
        if mm:
            vts[mm.group(1)] = True
    out = ['// generated by gen_bf.py: bit-field units of polymorphic classes (MWCC / clang order, see gen_bf.py)',
           'struct BfField { uint8_t lo, n; };', 'struct BfUnit { uint16_t off; uint8_t W, nf; const BfField* f; };', 'struct BfClass { const char* name; const char* vtsym; int nunits; const BfUnit* u; };']
    cls_rows = []
    for name, merged in classes.items():
        if name not in vts:
            continue
        flds = []; urows = []
        for base in sorted(merged):
            W, fl = merged[base]
            if base % 8:
                continue
            fl = sorted(set(fl))
            urows.append((base // 8, W, fl))
        out.append('static const BfField bf_f_%s[] = { %s };' % (name, ', '.join('{%d,%d}' % (lo, n) for _, _, fl in urows for lo, n in fl)))
        k = 0; ul = []
        for off, W, fl in urows:
            ul.append('{%d,%d,%d,&bf_f_%s[%d]}' % (off, W, len(fl), name, k)); k += len(fl)
        out.append('static const BfUnit bf_u_%s[] = { %s };' % (name, ', '.join(ul)))
        cls_rows.append('  { "%s", "_ZTV%d%s", %d, bf_u_%s },' % (name, len(name), name, len(urows), name))
    out.append('static const BfClass g_bfclasses[] = {'); out += cls_rows; out.append('  { nullptr, nullptr, 0, nullptr } };')
    open(os.path.join(outdir, 'dtl_bf.inc'), 'w').write('\n'.join(out) + '\n')
    print('gen_bf: %d polymorphic classes with bit-fields: %s' % (len(cls_rows), ' '.join(r.split('"')[1] for r in cls_rows)))


main()
