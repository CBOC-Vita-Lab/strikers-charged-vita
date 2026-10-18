#!/usr/bin/env python3
"""gen_dtl.py: generator of the live differential tester (see docs/wave3-NPG2.md).
   gen_dtl.py <native-exe> <outdir> [--roots TU,TU,...] [--report]
Writes <outdir>/dtl_gen.inc (translated retail bodies + dispatch wrappers), dtl_tables.inc (function / data / vtable tables), dtl_report.txt."""
import re, os, sys, subprocess, pickle, collections, argparse
HERE = os.path.dirname(os.path.abspath(__file__))
NP = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import mwcc, declscan

DECOMP = '/home/claude/research/decomp/mscharged-decomp/config/R4QE01'
SHARDS_INDEX = os.path.join(NP, 'tools', 'difftest', 'index.pkl')
SWEEP_TUS = ['NL/nlMath.cpp', 'NL/math.cpp', 'NL/plane.cpp', 'NL/utility.cpp', 'NL/plat/platqmath.cpp', 'NL/plat/platvmath.cpp', 'NL/nlIntersection.cpp', 'NL/nlPolygonRegion.cpp',
             'Game/AI/AiUtil.cpp', 'Game/Render/Frustum.cpp', 'Game/PoseNode.cpp', 'Game/SAnim.cpp', 'Game/SAnim/pnSAnimController.cpp', 'Game/PoseAccumulator.cpp', 'Game/SAnim/pnBlender.cpp',
             'Game/SAnim/pnSingleAxisBlender.cpp', 'Game/SAnim/pnFeather.cpp', 'Game/SAnim/pnScaleBlender.cpp', 'Game/SAnim/AnimRetargeter.cpp', 'Game/SAnimDecode.cpp',
             'Game/Physics/PhysicsFinitePlane.cpp', 'Game/Physics/PhysicsPlane.cpp', 'Game/Physics/PhysicsTransform.cpp', 'NL/glx/glxMatrix.cpp', 'NL/gl/glMatrix.cpp', 'NL/glx/glxSkinMatrix.cpp',
             'ode/matrix.cpp', 'ode/odemath.cpp', 'ode/util.cpp', 'ode/collision_util.cpp', 'ode/collision_transform.cpp']
DEFAULT_ROOTS = ['Game/Goalie.cpp', 'Game/AI/GoalieActions.cpp', 'Game/AI/GoalieLooseBall.cpp', 'Game/AI/GoalieSave.cpp', 'Game/GoalieFatigue.cpp']
# retail functions that must always be translated (no reliable native counterpart / ABI): addr (hex) or name
FORCE_TRANSLATE = set()
FORCE_NATIVE = set()


def load_overrides():
    p = os.path.join(HERE, 'dtl_overrides.txt')
    sig = {}
    if os.path.exists(p):
        for l in open(p):
            l = l.split('#')[0].strip()
            if not l:
                continue
            w = l.split()
            if w[0] == 'translate': FORCE_TRANSLATE.update(w[1:])
            elif w[0] == 'native': FORCE_NATIVE.update(w[1:])
            elif w[0] == 'sig': sig[w[1]] = w[2:]      # sig <retailname> <this|static> <kinds|-> <ret>
    return sig


def parse_retail():
    funcs, datas = {}, []
    for l in open(os.path.join(DECOMP, 'symbols.txt')):
        m = re.match(r'^(\S+) = \.(\w+):0x([0-9A-Fa-f]+); // (.*)$', l.strip())
        if not m:
            continue
        name, sec, addr, rest = m.group(1), m.group(2), int(m.group(3), 16), m.group(4)
        sz = re.search(r'size:0x([0-9A-Fa-f]+)', rest); sz = int(sz.group(1), 16) if sz else 0
        ty = re.search(r'type:(\w+)', rest); ty = ty.group(1) if ty else ''
        if ty == 'function':
            funcs.setdefault(addr, (name, sz))
        elif sec in ('data', 'bss', 'sdata', 'sbss', 'sbss2', 'rodata', 'sdata2'):
            datas.append((addr, sz, name, sec))
    return funcs, datas


def tu_ranges():
    cur = None; rng = {}
    for l in open(os.path.join(DECOMP, 'splits.txt')):
        m = re.match(r'^(\S+\.cpp):', l)
        if m and not l.startswith('\t'):
            cur = m.group(1); continue
        m = re.match(r'^\s+\.text\s+start:0x([0-9A-F]+) end:0x([0-9A-F]+)', l)
        if m and cur:
            rng[cur] = (int(m.group(1), 16), int(m.group(2), 16))
    return rng


def native_symbols(exe):
    out = subprocess.run(['nm', '-S', '--defined-only', '-g', exe], capture_output=True, text=True).stdout
    funcs, datas, sizes = {}, {}, {}
    for l in out.split('\n'):
        w = l.split()
        if len(w) == 4:
            sizes[w[3]] = int(w[1], 16); w = [w[0], w[2], w[3]]
        if len(w) != 3:
            continue
        t = w[1]
        if t in 'TWwi':
            funcs[w[2]] = int(w[0], 16)
        elif t in 'DBVRrdbGSsC':
            datas[w[2]] = int(w[0], 16)
    # local (file-static) data symbols: lower-case types; PIE: addresses are relative to the load base (dtl.cpp adds the bias of a reference global)
    out2 = subprocess.run(['nm', '-S', '--defined-only', exe], capture_output=True, text=True).stdout
    locs = collections.defaultdict(list)
    for l in out2.split('\n'):
        w = l.split()
        if len(w) != 4 or w[2] not in ('d', 'b', 'r', 's'):
            continue
        n = w[3]
        m = re.match(r'^_ZL(\d+)(\w+)$', n)
        if m and len(m.group(2)) == int(m.group(1)):
            n = m.group(2)
        elif n.startswith('_ZZ'):
            mz = re.search(r'E(\d+)(\w+?)(?:_\d+)?$', n)           # function-local static: the last identifier
            if not mz or len(mz.group(2)) < int(mz.group(1)):
                continue
            n = mz.group(2)[:int(mz.group(1))]
        elif n.startswith('_Z') or '.' in n or '$' in n:
            continue
        locs[n].append((int(w[0], 16), int(w[1], 16)))
    return funcs, datas, sizes, locs


class Tool:
    def __init__(self, exe):
        self.rf, self.rd = parse_retail()
        self.rng = tu_ranges()
        self.nf, self.nd, self.nsz, self.nlocs = native_symbols(exe)
        self.dem = mwcc.demangle_native([s for s in self.nf if s.startswith('_Z')])
        self.by_qk = collections.defaultdict(list); self.by_q = collections.defaultdict(list)
        for s, d in self.dem.items():
            p = mwcc.native_parse(d)
            if not p:
                continue
            q, kinds = p
            self.by_qk[(q, tuple(kinds))].append(s); self.by_q[q].append(s)
        ds = [s for s in self.nd if s.startswith('_Z')]
        self.ddem = {}
        for s, d in mwcc.demangle_native(ds).items():
            self.ddem.setdefault(d, s)
        self.decl = declscan.scan(NP)
        mwcc.STRUCT_BY_VALUE.update(['nlVector3', 'nlVector2', 'nlVector4', 'nlMatrix4'])   # refined below by scanning for by-value uses
        self.idx = pickle.load(open(SHARDS_INDEX, 'rb'))
        self.cache = {}
        # the recompiler's "fast reference" set: baseline translations of the functions it replaces by hand-written host code (fastref_<ADDR>)
        fr = '/home/claude/msc/split/gen-trunk/fast_reference/fast_reference.cpp'
        if os.path.exists(fr):
            lines = open(fr, errors='replace').read().split('\n'); self.cache[fr] = lines
            marks = [(i, int(m.group(1), 16)) for i, l in enumerate(lines) for m in [re.match(r'extern "C" void fastref_([0-9A-F]{8})\(CpuContext\* MKW_RESTRICT ctx\)', l)] if m]
            for k, (i, a) in enumerate(marks):
                end = marks[k + 1][0] if k + 1 < len(marks) else len(lines)
                self.idx.setdefault(a, (fr, i, end))
        self.sigov = load_overrides()
        pk = os.path.join(HERE, 'pairedret.pkl')
        if not os.path.exists(pk):
            subprocess.run([sys.executable, os.path.join(HERE, 'pairedret.py')], check=True)
        self.pairedret = pickle.load(open(pk, 'rb'))
        self.shim_nat = {}

    # --- signature -------------------------------------------------------------------------------------------
    def kind_from_decl(self, p):
        p = re.sub(r'=.*$', '', p).strip()
        if re.search(r'\b(dVector3|dVector4|dMatrix3|dMatrix4|dMatrix6|dQuaternion)\b', p) and '*' not in p:
            return 'p'                        # ODE array typedefs decay to pointers
        if '*' in p or '&' in p or '(' in p:
            return 'p'
        toks = re.sub(r'\bconst\b|\bvolatile\b|\bstruct\b|\bclass\b|\benum\b', '', p).split()
        if not toks:
            return 'i'
        base = ' '.join(toks[:-1]) if len(toks) > 1 and toks[-1] not in ('int', 'long', 'short', 'char', 'float', 'double', 'bool') else ' '.join(toks)
        base = base.strip()
        if base in ('float', 'f32', 'dReal'): return 'f'
        if base in ('double', 'f64'): return 'd'
        if base in ('long long', 'unsigned long long', 's64', 'u64'): return 'L'
        if base == '...': return '.'
        return 'S' if base in mwcc.STRUCT_BY_VALUE else 'i'

    def ret_kind(self, cls, name, npar=None):
        keys = ([cls + '::' + name] if cls else []) + [name]
        for k in keys:
            ent = [(r, n) for (r, n) in self.decl.ret.get(k, []) if r and r not in ('default:', 'return', 'else')]
            if npar is not None and any(n == npar for _, n in ent):
                ent = [(r, n) for (r, n) in ent if n == npar]
            rts = [r for r, _ in ent]
            if rts:
                r = collections.Counter(rts).most_common(1)[0][0]
                r = re.sub(r'\b(const|inline|static|virtual|extern|"C")\b', '', r).strip()
                if r.endswith('*') or r.endswith('&'): return 'p'
                if r in ('void',): return 'v'
                if r in ('bool', 'char', 'unsigned char', 'u8', 's8', 'signed char'): return 'b' if r == 'bool' else ('u' if r in ('unsigned char', 'u8') else 'c')
                if r in ('short', 's16'): return 's'
                if r in ('unsigned short', 'u16'): return 'w'
                if r in ('float', 'f32'): return 'f'
                if r in ('double', 'f64'): return 'd'
                if r in ('long long', 'unsigned long long', 's64', 'u64'): return 'L'
                if r in ('int', 'unsigned int', 'long', 'unsigned long', 'u32', 's32', 'unsigned', 'size_t'): return 'i'
                if re.match(r'^[a-z]?[A-Z]\w*$', r) or r in ('nlVector3',):
                    return 'S' if r in mwcc.STRUCT_BY_VALUE else 'i'
                return 'i'
        return '?'

    SPECIAL = {'__dl__FPv': ('_ZdlPv', ['p'], 'v'), '__dla__FPv': ('_ZdaPv', ['p'], 'v'), '__nw__FUl': ('_Znwj', ['i'], 'p'), '__nwa__FUl': ('_Znaj', ['i'], 'p'),
               'memcpy': ('memcpy', ['p', 'p', 'i'], 'p'), 'memmove': ('memmove', ['p', 'p', 'i'], 'p'), 'memset': ('memset', ['p', 'i', 'i'], 'p'),
               'strlen': ('strlen', ['p'], 'i'), 'strcmp': ('strcmp', ['p', 'p'], 'i'), 'strncmp': ('strncmp', ['p', 'p', 'i'], 'i'),
               'strcpy': ('strcpy', ['p', 'p'], 'p'), 'strncpy': ('strncpy', ['p', 'p', 'i'], 'p'),
               'sqrtf': ('sqrtf', ['f'], 'f'), 'sinf': ('sinf', ['f'], 'f'), 'cosf': ('cosf', ['f'], 'f'), 'tanf': ('tanf', ['f'], 'f'),
               'atan2f': ('atan2f', ['f', 'f'], 'f'), 'fabsf': ('fabsf', ['f'], 'f'), 'floorf': ('floorf', ['f'], 'f'), 'ceilf': ('ceilf', ['f'], 'f'), 'powf': ('powf', ['f', 'f'], 'f')}
    LIBC = {'memcpy', 'memmove', 'memset', 'strlen', 'strcmp', 'strncmp', 'strcpy', 'strncpy', 'sqrtf', 'sinf', 'cosf', 'tanf', 'atan2f', 'fabsf', 'floorf', 'ceilf', 'powf'}

    def resolve_native(self, name):
        """retail symbol -> dict(nat=<mangled or None>, kinds=[...], this=bool, ret=char) ; dict has 'why' on failure"""
        d = mwcc.demangle(name)
        res = dict(nat=None, kinds=None, this=False, ret='?', why='', ptypes=[])
        if name in self.SPECIAL:
            n, k, r = self.SPECIAL[name]
            if n in self.nf or name in self.LIBC:
                res.update(nat=n, kinds=k, ret=r)
                return res
        if d is None:
            # unmangled: C function / fn_XXXXXXXX / SDK
            res['kinds'] = None
            nm = name
            if nm in self.nf:
                res['nat'] = nm
            else:
                c = [s for s, dd in self.dem.items() if dd.startswith(nm + '(')]
                if len(c) == 1:
                    res['nat'] = c[0]
            pr = self.decl.proto.get(nm)
            if pr:
                ps = None
                for p in pr:
                    if p.strip():
                        ps = p; break
                ps = ps if ps is not None else ''
                if ps.strip() in ('', 'void'):
                    res['kinds'] = []
                else:
                    parts = mwcc.split_top(ps)
                    res['kinds'] = [self.kind_from_decl(x) for x in parts]
                    res['ptypes'] = [('Pf' if re.search(r'dReal|dVector|dMatrix|dQuaternion|float|f32', x) else 'P?') if k == 'p' else ('f' if k == 'f' else 'i') for x, k in zip(parts, res['kinds'])]
            res['ret'] = self.ret_kind('', nm, None if res['kinds'] is None else len(res['kinds']))
            if res['kinds'] is None:
                res['why'] = 'no prototype'
            return res
        cls = d['cls']; nm = d['name']
        qual = '::'.join(cls + [nm])
        if nm == '__ct': qual = '::'.join(cls + [cls[-1]])
        elif nm == '__dt': qual = '::'.join(cls + ['~' + cls[-1]])
        kinds = d['params']
        res['kinds'] = kinds; res['ptypes'] = d.get('ptypes', [])
        clsname = '::'.join(cls)
        is_static = bool(cls) and (clsname + '::' + nm) in self.decl.static
        res['this'] = bool(cls) and not is_static
        res['ret'] = self.ret_kind(cls[-1] if cls else '', nm, len(kinds))
        cand = self.by_qk.get((qual, tuple(k if k != 'S' else 'i' for k in kinds)), [])
        if not cand:
            cand = self.by_qk.get((qual, tuple(kinds)), [])
        if not cand:
            c2 = self.by_q.get(qual, [])
            c3 = [s for s in c2 if len(mwcc.native_parse(self.dem[s])[1]) == len(kinds)]
            cand = c3 if len(c3) == 1 else ([] if len(c2) != 1 else c2)
        if len(cand) > 1 or (len(cand) == 1 and d.get('ptypes')):
            # overloads with equal ABI kinds (nlVector2 / nlVector3 versions ...): the pointee type names decide
            want = mwcc.mwcc_pointee_names(d.get('ptypes', []))
            best = [s for s in cand if mwcc.native_pointee_names(self.dem[s]) == want]
            if best:
                cand = best
            elif len(cand) > 1 or any(w is not None for w in want):
                res['why'] = 'param types differ: want %s have %s' % (want, [mwcc.native_pointee_names(self.dem[s]) for s in cand])
                if len(cand) == 1:
                    cand = []
        if len(cand) >= 1:
            res['nat'] = sorted(cand)[0]
            if len(cand) > 1:
                res['why'] = 'ambiguous:' + ','.join(cand)
        else:
            res['why'] = 'no native symbol'
        return res

    # --- translated text --------------------------------------------------------------------------------------
    def text(self, a):
        f, s, e = self.idx[a]
        if f not in self.cache:
            self.cache[f] = open(f, errors='replace').read().split('\n')
        lines = self.cache[f][s:e]
        while lines and (lines[-1].startswith('// RECOMP_GUEST_ABI') or not lines[-1].strip()):
            lines.pop()
        t = '\n'.join(lines)
        t = re.sub(r'(?m)^#(include|undef|line|define)\b.*$', '', t)
        return t

    @staticmethod
    def callees(t):
        c = set(int(x, 16) for x in re.findall(r'MKW_STATIC_TRANSLATED_CALL\(0x([0-9A-Fa-f]{8})u', t))
        c |= set(int(x, 16) for x in re.findall(r'InvokeDirectCpu<0x([0-9A-Fa-f]{8})u>', t))
        c |= set(int(x, 16) for x in re.findall(r'(?<![\w])(?:func|fastref)_([0-9A-F]{8})\(ctx\);', t))
        return c


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exe'); ap.add_argument('outdir')
    ap.add_argument('--roots', default=','.join(DEFAULT_ROOTS))
    ap.add_argument('--report', action='store_true')
    ap.add_argument('--sweep', action='store_true', help='also root the numeric TUs of SWEEP_TUS and emit g_sweep')
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    T = Tool(a.exe)
    roots = []
    sweep_roots = []
    tus = a.roots.split(',')
    if a.sweep:
        tus += [t for t in SWEEP_TUS if t in T.rng and t not in tus]
    for tu in tus:
        lo, hi = T.rng[tu]
        rr = [x for x in T.rf if lo <= x < hi and not T.rf[x][0].startswith('__sinit_')]
        roots += rr
        if tu in SWEEP_TUS: sweep_roots += rr
    roots = sorted(set(roots))
    sweep_set = set(sweep_roots)
    main_set = set(roots) - sweep_set
    trans = {}      # addr -> info
    info = {}
    work = list(roots)
    unresolved = {}
    def rinfo(addr):
        if addr in info:
            return info[addr]
        name = T.rf.get(addr, ('func_%08X' % addr, 0))[0]
        r = T.resolve_native(name)
        if name in T.sigov:
            w = T.sigov[name]
            r['this'] = (w[0] == 'this'); r['kinds'] = [] if w[1] == '-' else list(w[1]); r['ret'] = w[2]; r['why'] = ''
        r['name'] = name
        info[addr] = r
        return r
    seen = set(roots)
    while work:
        addr = work.pop()
        if addr not in T.idx:
            unresolved[addr] = 'no translation in shards'
            continue
        trans[addr] = True
        t = T.text(addr)
        for c in sorted(Tool.callees(t)):
            if c in seen:
                continue
            seen.add(c)
            r = rinfo(c)
            use_native = r['nat'] is not None and r['kinds'] is not None and 'S' not in r['kinds'] and r['ret'] != 'S' \
                and r['name'] not in FORCE_TRANSLATE and ('%08x' % c) not in FORCE_TRANSLATE
            if r['name'] in FORCE_NATIVE or ('%08x' % c) in FORCE_NATIVE:
                use_native = True
            if not use_native:
                work.append(c)       # translate it as well
    # report
    rep = []
    rep.append('roots: %d, translated set: %d, functions with info: %d' % (len(roots), len(trans), len(info)))
    for c in sorted(set(info) | set(roots)):
        r = rinfo(c)
        tag = 'T' if c in trans else 'N'
        rep.append('%s %08x %-70s nat=%s kinds=%s this=%s ret=%s %s' % (tag, c, r['name'][:70], (r['nat'] or '-')[:60], ''.join(r['kinds']) if r['kinds'] is not None else '?', int(r['this']), r['ret'], r['why']))
    for c, w in sorted(unresolved.items()):
        rep.append('UNRESOLVED %08x %s: %s' % (c, T.rf.get(c, ('?',))[0], w))
    open(os.path.join(a.outdir, 'dtl_report.txt'), 'w').write('\n'.join(rep) + '\n')
    print('\n'.join(rep[:1]))
    # --- emit ------------------------------------------------------------------------------------------------
    allfn = sorted(set(roots) | set(info) | set(trans))
    with open(os.path.join(a.outdir, 'dtl_gen.inc'), 'w') as f:
        f.write('// GENERATED by gen_dtl.py\n')
        for c in sorted(trans):
            t = T.text(c)
            t = re.sub(r'extern "C" void (func|fastref)_%08X(__impl)?\(' % c, 'extern "C" void tr_%08X(' % c, t)
            t = re.sub(r'(?<![\w])(?:func|fastref)_([0-9A-F]{8})\(ctx\);', r'dtl_dispatch(0x\1u, ctx);', t)
            f.write(t + '\n')
        f.write('static const struct { uint32_t a; void (*f)(CpuContext*); } g_translated[] = {\n')
        for c in sorted(trans):
            f.write('  { 0x%08Xu, tr_%08X },\n' % (c, c))
        f.write('  { 0u, nullptr } };\n')
    with open(os.path.join(a.outdir, 'dtl_tables.inc'), 'w') as f:
        f.write('// GENERATED by gen_dtl.py\nstatic const DtlFn g_fns[] = {\n')
        for c in allfn:
            r = rinfo(c)
            kinds = ''.join(r['kinds']) if r['kinds'] is not None else '?'
            f.write('  { 0x%08Xu, "%s", %s, "%s", %d, \'%s\', %d, "%s", %d },\n' % (c, r['name'].replace('\\', '\\\\').replace('"', '\\"'),
                    ('"%s"' % r['nat']) if r['nat'] else 'nullptr', kinds, int(r['this']), r['ret'] if r['ret'] != '?' else 'i', (2 if c in sweep_set and c not in main_set else (1 if c in roots else 0)), ';'.join(r.get('ptypes', [])), 1 if c in T.pairedret else 0))
        f.write('  { 0u, nullptr, nullptr, nullptr, 0, 0, 0, nullptr, 0 } };\n')
        # data symbols with a native counterpart (mutable sections)
        f.write('static const DtlData g_data[] = {\n')
        n = 0
        for (addr, sz, name, sec) in sorted(T.rd):
            if sec not in ('data', 'bss', 'sdata', 'sbss', 'sbss2'):
                continue
            nat = None
            if name in T.nd:
                nat = name
            else:
                d = re.match(r'^(\w+)__(\d+)(\w+)$', name)
                if d and not name.startswith('__vt__'):
                    ln = int(d.group(2)); cn = d.group(3)
                    if len(cn) == ln:
                        cand = '_ZN%d%s%d%sE' % (ln, cn, len(d.group(1)), d.group(1))
                        if cand in T.nd: nat = cand
                if name.startswith('__vt__'):
                    cn = name[6:]
                    m = re.match(r'^(\d+)(\w+)$', cn)
            if not nat and not name.startswith('__vt__'):
                mt = re.match(r'^(\w+)__(\d+)(.+)$', name)           # class statics of (template) classes: 'member__<len>Class<args>' -> 'Class<Args>::member' (c++filt text)
                if mt and len(mt.group(3)) == int(mt.group(2)):
                    cls = re.sub(r'(?<=[<,\s])\d+(?=[A-Za-z_])', '', mt.group(3))
                    nat = T.ddem.get(cls + '::' + mt.group(1))
            if name.startswith('__vt__'):
                m = re.match(r'^__vt__(\d+)(\w+)$', name)
                if m and len(m.group(2)) == int(m.group(1)):
                    cand = '_ZTV%d%s' % (int(m.group(1)), m.group(2))
                    if cand in T.nd: nat = cand
            if nat:
                f.write('  { 0x%08Xu, %d, "%s", %d },\n' % (addr, sz, nat, 1 if name.startswith('__vt__') else 0)); n += 1
        f.write('  { 0u, 0, nullptr, 0 } };\n')
        # file-static data of the native build that has the retail symbol's name (unique, or an immutable constant whose copies are all equal)
        f.write('static const DtlLocal g_locals[] = {\n')
        nl = 0
        for (addr, sz, name, sec) in sorted(T.rd):
            if name in T.nd or name.startswith('@') or name.startswith('__vt__'):
                continue
            c = T.nlocs.get(name) or (T.nlocs.get(name.split('$')[0]) if '$' in name else None)
            if not c:
                continue
            if len(c) > 1 and sec not in ('rodata', 'sdata2'):
                continue
            if len(c) > 1 and len(set(sz_ for _, sz_ in c)) > 1:
                continue
            if sz and c[0][1] and sz != c[0][1]:
                continue                       # same name, different size: another object
            f.write('  { 0x%08Xu, %d, "%s" },\n' % (addr, max(sz, 1), name)); nl += 1
        f.write('  { 0u, 0, nullptr } };\n')
        ref = next((s for s in ('g_pTeams', 'g_pBall', 'g_pGame') if s in T.nd), None)
        f.write('static const char* const g_localref = "%s";\n' % ref)
        print('file-static data symbols mapped:', nl)
        f.write('static const DtlVt g_vts[] = {\n')
        for s in sorted(T.nd):
            if s.startswith('_ZTV') and s in T.nsz:
                f.write('  { "%s", %d },\n' % (s, T.nsz[s]))
        f.write('  { nullptr, 0 } };\n')
        f.write('static const DtlSym g_rfuncs[] = {\n')                    # every retail function (names of the virtual slots)
        for a in sorted(T.rf):
            nm_, sz_ = T.rf[a][0], T.rf[a][1]
            f.write('  { 0x%08Xu, %d, "%s" },\n' % (a, sz_, nm_.replace('\\', '\\\\').replace('"', '\\"')))
        f.write('  { 0u, 0, nullptr } };\n')
        f.write('static const DtlSym g_rsyms[] = {\n')
        for (addr, sz, name, sec) in sorted(T.rd):
            if sec in ('data', 'bss', 'sdata', 'sbss', 'sbss2') and not name.startswith('@'):
                f.write('  { 0x%08Xu, %d, "%s" },\n' % (addr, sz, name.replace('\\', '\\\\').replace('"', '\\"')))
        f.write('  { 0u, 0, nullptr } };\n')
        enum = {}
        for l in open(os.path.join(NP, 'include', 'Game', 'Goalie.h'), errors='replace'):
            m = re.match(r'\s*(GOALIEACTION_\w+)\s*=\s*(\d+)', l)
            if m: enum[m.group(1)] = int(m.group(2))
        src = open(os.path.join(NP, 'src', 'Game', 'Goalie.cpp'), errors='replace').read()
        f.write('static const uint32_t g_sweep[] = {\n')
        for c in sorted(set(sweep_roots)): f.write('  0x%08Xu,\n' % c)
        f.write('  0u };\n')
        f.write('static const struct { const char* fn; int state; } g_fnstate[] = {\n')
        for m in re.finditer(r'case (GOALIEACTION_\w+):\s*(\w+)\(fDeltaTime\);', src):
            if m.group(1) in enum:
                f.write('  { "%s", %d },\n' % (m.group(2), enum[m.group(1)]))
        f.write('  { nullptr, 0 } };\n')
        f.write('static const DtlField g_fields[] = {\n')
        seen = {}
        for hdr in ('Goalie.h', 'Player.h', 'Character.h'):
            for l in open(os.path.join(NP, 'include', 'Game', hdr), errors='replace'):
                m = re.match(r'\s*/\*\s*0x([0-9A-Fa-f]+)\s*\*/\s*[\w:<>\*& ]+?\s+\**(\w+)(\[\w+\])?\s*;', l)
                if m:
                    off = int(m.group(1), 16)
                    if off not in seen:
                        seen[off] = m.group(2); f.write('  { 0x%X, "%s" },\n' % (off, m.group(2)))
        f.write('  { 0xFFFFFFFFu, nullptr } };\n')
    print('data symbols with native counterpart:', n)


if __name__ == '__main__':
    main()
