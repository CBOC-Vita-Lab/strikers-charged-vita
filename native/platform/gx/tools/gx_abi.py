#!/usr/bin/env python3
"""ABI conformance of the game's GX API (decomp's revolution/*.h) against vgx's API (dolphin/*.h of Aurora).

  gx_abi.py [--rvl DIR] [--aurora DIR] [--calls FILE] [--out DIR]

Parses both header sets with clang (-m32, AST as JSON), takes the list of SDK functions the game calls
(--calls: one name per line, made by gx_calls.py), and reports per function:
  same     identical normalised prototype in both sets (enums = int, typedefs resolved, pointers = ptr)
  differs  both declare it, prototype differs (the glue must define it)
  absent   vgx does not declare it (the glue must define it)
It also compares the value of every GX_* enumerator and sizeof of the public structs the game passes.
Needs only clang. Writes <out>/abi_functions.tsv, abi_enums.txt, abi_structs.txt.
"""
import argparse, json, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
RVL = '/home/claude/research/decomp/mscharged-decomp/libs/RVL_SDK/include'
AUR = '/home/claude/msc/one/port/aurora-main/include'

RVL_PROBE = '#include <revolution/gx.h>\n#include <revolution/vi.h>\n#include <revolution/tpl.h>\n'
AUR_PROBE = 'typedef int BOOL;\n#include <dolphin/gx.h>\n#include <dolphin/gx/GXAurora.h>\n#include <dolphin/gx/GXExtra.h>\n#include <dolphin/vi.h>\n'

def clang_ast(probe, incs, defs, lang='c'):
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, 'probe.' + ('c' if lang == 'c' else 'cpp'))
        open(p, 'w').write(probe)
        cmd = ['clang', '-m32', '-fsyntax-only', '-x', lang, '-Xclang', '-ast-dump=json'] + defs + [f'-I{i}' for i in incs] + [p]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            sys.stderr.write(r.stderr[:3000])
            raise SystemExit('clang failed')
        return json.loads(r.stdout)

def walk(node, fn):
    fn(node)
    for k in node.get('inner', []) or []:
        walk(k, fn)

def make_resolver(typedefs, enums_t):
    def res(t, depth=0):
        t = re.sub(r'\b(const|volatile|restrict)\b', '', t)
        t = re.sub(r'\s+', ' ', t).strip()
        if depth > 8:
            return t
        m = re.fullmatch(r'(.*?)((?:\s*\*|\s*\[[^\]]*\]|\s*\([^)]*\))*)', t)
        base, suffix = (m.group(1).strip(), m.group(2)) if m else (t, '')
        if re.search(r'\*', suffix):
            return 'ptr'
        if base in typedefs and not base.startswith('struct '):
            return res(typedefs[base] + suffix, depth + 1)
        b = base
        if b.startswith('enum '):
            return 'int'
        if b.startswith('struct ') or b.startswith('union '):
            return 'struct ' + re.sub(r'^_+', '', b.split(' ', 1)[1])
        b = re.sub(r'\bunsigned (long|int)\b', 'u32', b)
        b = re.sub(r'\bsigned (long|int)\b', 's32', b)
        b = re.sub(r'\b(long|int)\b', 's32', b)
        b = re.sub(r'\bunsigned short\b', 'u16', b)
        b = re.sub(r'\bshort\b', 's16', b)
        b = re.sub(r'\bunsigned char\b', 'u8', b)
        b = re.sub(r'\b(signed char|char)\b', 's8', b)
        b = re.sub(r'\b_?Bool\b|\bbool\b', 'u8', b)
        return b.strip()
    return res

def collect(ast):
    funcs, enums, typedefs = {}, {}, {}
    def tds(n):
        if n.get('kind') == 'TypedefDecl' and n.get('name'):
            typedefs.setdefault(n['name'], n['type']['qualType'])
    walk(ast, tds)
    # an anonymous enum / struct typedef: the typedef's own type is 'enum (unnamed ...)' -> int / struct name
    for k, v in list(typedefs.items()):
        if 'unnamed' in v or 'anonymous' in v:
            typedefs[k] = 'enum x' if v.startswith('enum') else 'struct ' + k
    res = make_resolver(typedefs, None)
    def visit(n):
        k = n.get('kind')
        if k == 'FunctionDecl' and n.get('name'):
            params = [res(c['type']['qualType']) for c in (n.get('inner') or []) if c.get('kind') == 'ParmVarDecl']
            ret = res(n['type']['qualType'].split('(')[0].strip())
            funcs.setdefault(n['name'], (ret, tuple(params), n.get('storageClass') == 'static'))
        elif k == 'EnumDecl':
            prev = -1
            for c in n.get('inner') or []:
                if c.get('kind') != 'EnumConstantDecl':
                    continue
                val = None
                for x in c.get('inner') or []:
                    if x.get('kind') == 'ConstantExpr' and 'value' in x:
                        val = int(x['value'])
                        break
                if val is None:
                    val = prev + 1
                prev = val
                enums.setdefault(c['name'], val)
    walk(ast, visit)
    return funcs, enums

def sizes(probe, incs, defs, names, lang='c'):
    src = probe + '#include <stdio.h>\nint main(void){\n'
    for n in names:
        src += f'printf("{n} %u\\n", (unsigned)sizeof({n}));\n'
    src += 'return 0;}\n'
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, 's.c' if lang == 'c' else 's.cpp'); open(p, 'w').write(src)
        exe = os.path.join(d, 's')
        r = subprocess.run(['clang', '-m32', '-x', lang, '-w'] + defs + [f'-I{i}' for i in incs] + [p, '-o', exe], capture_output=True, text=True)
        if r.returncode != 0:
            return {n: None for n in names}
        out = subprocess.run([exe], capture_output=True, text=True).stdout
        return {l.split()[0]: int(l.split()[1]) for l in out.splitlines()}

STRUCTS = ['GXColor', 'GXColorS10', 'GXTexObj', 'GXTlutObj', 'GXLightObj', 'GXFifoObj', 'GXRenderModeObj', 'GXTexRegion', 'GXTlutRegion']

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rvl', default=RVL); ap.add_argument('--aurora', default=AUR)
    ap.add_argument('--shim', default=os.path.join(HERE, 'shim'))
    ap.add_argument('--calls', default=None); ap.add_argument('--out', default='.')
    a = ap.parse_args()
    rdefs = ['-Dchar16_t=rvl_char16_t']
    rincs = [a.shim, os.path.join(HERE, '..', '..', '..', 'include'), a.rvl]
    rf, re_ = collect(clang_ast(RVL_PROBE, rincs, rdefs))
    adefs = ['-DTARGET_PC']   # how vgx itself is built: its GXTexObj / GXTlutObj are the larger host objects
    af, _ = collect(clang_ast(AUR_PROBE, [a.aurora], adefs, lang='c++'))   # functions as vgx compiles them (C++, TARGET_PC)
    _, ae = collect(clang_ast(AUR_PROBE, [a.aurora], []))                  # enumerators from the C view (the numbers are the same in both)
    calls = [l.strip() for l in open(a.calls)] if a.calls else sorted(set(rf) & set(af))
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, 'abi_functions.tsv'), 'w') as f:
        f.write('function\tstatus\trvl prototype\tvgx (aurora) prototype\n')
        cnt = {}
        for n in calls:
            r, v = rf.get(n), af.get(n)
            if r is None: st = 'not-in-rvl-headers'
            elif v is None: st = 'absent'
            elif r[:2] == v[:2]: st = 'same'
            else: st = 'differs'
            cnt[st] = cnt.get(st, 0) + 1
            fmt = lambda x: '' if x is None else f'{x[0]} ({", ".join(x[1])})'
            f.write(f'{n}\t{st}\t{fmt(r)}\t{fmt(v)}\n')
        sys.stderr.write('functions: ' + ', '.join(f'{k} {v}' for k, v in sorted(cnt.items())) + '\n')
    bad = 0
    with open(os.path.join(a.out, 'abi_enums.txt'), 'w') as f:
        common = sorted(set(re_) & set(ae))
        for n in common:
            if re_[n] != ae[n]:
                bad += 1
                f.write(f'DIFF {n} rvl={re_[n]} aurora={ae[n]}\n')
        f.write(f'# {len(common)} enumerators in both, {bad} differ; only in rvl: {len(set(re_) - set(ae))}, only in aurora: {len(set(ae) - set(re_))}\n')
        f.write('# only in rvl (game may use): ' + ' '.join(sorted(set(re_) - set(ae))) + '\n')
    sys.stderr.write(f'enumerators: {len(common)} common, {bad} differ\n')
    rs = sizes(RVL_PROBE, rincs, rdefs, STRUCTS); as_ = sizes(AUR_PROBE, [a.aurora], adefs, STRUCTS, 'c++')
    with open(os.path.join(a.out, 'abi_structs.txt'), 'w') as f:
        for n in STRUCTS:
            note = ''
            if rs.get(n) != as_.get(n) and n in ('GXTexObj', 'GXTlutObj'):
                note = '  (the glue defines the texture / palette object functions itself: compact objects, expanded at GXLoadTexObj / GXLoadTlut)'
            f.write(f'{n}\trvl={rs.get(n)}\taurora={as_.get(n)}\t{"same" if rs.get(n) == as_.get(n) else "DIFFERS"}{note}\n')

if __name__ == '__main__':
    main()
