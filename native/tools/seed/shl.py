#!/usr/bin/env python3
"""W5-SEED: the vgx shader warm-up list (vgx_shaderlist.bin / MSC_VGX_SHADERLIST) as a set of keys.

File format (vgx src/vgx_draw.cpp, EnsureShaderList):
  header   char magic[8]="VGXSHL01"; u32 vsKeySize, fsKeySize, genVersion, reserved
  records  u32 type, payload:  1 = VsKey bytes, 2 = FsKey bytes (ids = order in the file),
           3 = {u32 vsId, u32 fsId} a program, 4 = {u32 vsId, fsId, layoutMask, blendKey} a program variant.

Commands:
  shl.py info   a.shl [b.shl ...]           counts per file
  shl.py merge  -o out.shl a.shl b.shl ...  union by key content (ids renumbered), deterministic order
  shl.py diff   base.shl new.shl            what new.shl has that base.shl does not
  shl.py table  [--base seed.shl] a.shl b.shl ...   per file: keys used, and NEW keys over the union of the files before it
  shl.py decode a.shl                       one line per shader key (stages, texgens, ...)
  shl.py check  a.shl                       structure check (ids in range, no duplicates), as the C++ reader requires
  shl.py sub    -o out.shl --novariants a.shl   copy without the type 4 records (or --maxvar N)
  shl.py split  --parts N -o prefix a.shl   N lists that together hold everything (programs dealt out in order, each with the shaders it needs and its variants):
                                            for a warm-up that does not fit one Vita3K turn; the cache of the parts adds up
"""
import argparse, struct, sys, os

MAGIC = b'VGXSHL01'


class SList:
    def __init__(self):
        self.vsSize = self.fsSize = self.gen = None
        self.vs = []      # key bytes, by id
        self.fs = []
        self.prog = []    # (vsKey, fsKey)
        self.var = []     # (vsKey, fsKey, layout, blend)
        self._vsIdx = {}
        self._fsIdx = {}
        self._prog = set()
        self._var = set()
        self.dups = 0

    def add_vs(self, k):
        if k in self._vsIdx:
            self.dups += 1
            return False
        self._vsIdx[k] = len(self.vs); self.vs.append(k); return True

    def add_fs(self, k):
        if k in self._fsIdx:
            self.dups += 1
            return False
        self._fsIdx[k] = len(self.fs); self.fs.append(k); return True

    def add_prog(self, v, f):
        if (v, f) in self._prog:
            self.dups += 1
            return False
        self._prog.add((v, f)); self.prog.append((v, f)); return True

    def add_var(self, v, f, lay, bl):
        t = (v, f, lay, bl)
        if t in self._var:
            self.dups += 1
            return False
        self._var.add(t); self.var.append(t); return True

    def has_vs(self, k): return k in self._vsIdx
    def has_fs(self, k): return k in self._fsIdx
    def has_prog(self, v, f): return (v, f) in self._prog
    def has_var(self, v, f, lay, bl): return (v, f, lay, bl) in self._var


def read(path):
    d = open(path, 'rb').read()
    if len(d) < 24 or d[:8] != MAGIC:
        raise ValueError('%s: not a shader list' % path)
    L = SList()
    _, L.vsSize, L.fsSize, L.gen, _r = (None,) + struct.unpack_from('<IIII', d, 8)
    p = 24
    while p + 4 <= len(d):
        t, = struct.unpack_from('<I', d, p); p += 4
        if t == 1:
            L.add_vs(d[p:p + L.vsSize]); p += L.vsSize
        elif t == 2:
            L.add_fs(d[p:p + L.fsSize]); p += L.fsSize
        elif t == 3:
            v, f = struct.unpack_from('<II', d, p); p += 8
            L.add_prog(L.vs[v], L.fs[f])
        elif t == 4:
            v, f, lay, bl = struct.unpack_from('<IIII', d, p); p += 16
            L.add_var(L.vs[v], L.fs[f], lay, bl)
        else:
            raise ValueError('%s: bad record type %d at %d' % (path, t, p - 4))
    if p != len(d):
        raise ValueError('%s: %d trailing bytes' % (path, len(d) - p))
    return L


def write(L, path, novariants=False, maxvar=None):
    out = bytearray(MAGIC + struct.pack('<IIII', L.vsSize, L.fsSize, L.gen, 0))
    for k in L.vs: out += struct.pack('<I', 1) + k
    for k in L.fs: out += struct.pack('<I', 2) + k
    vi = {k: i for i, k in enumerate(L.vs)}
    fi = {k: i for i, k in enumerate(L.fs)}
    for v, f in L.prog: out += struct.pack('<III', 3, vi[v], fi[f])
    if not novariants:
        n = 0
        for v, f, lay, bl in L.var:
            if maxvar is not None and n >= maxvar: break
            out += struct.pack('<IIIII', 4, vi[v], fi[f], lay, bl); n += 1
    tmp = path + '.tmp'
    open(tmp, 'wb').write(out)
    os.replace(tmp, path)


def merge(lists):
    U = SList()
    for L in lists:
        if U.vsSize is None:
            U.vsSize, U.fsSize, U.gen = L.vsSize, L.fsSize, L.gen
        elif (U.vsSize, U.fsSize, U.gen) != (L.vsSize, L.fsSize, L.gen):
            raise ValueError('lists from different generators: %s vs %s' % ((U.vsSize, U.fsSize, U.gen), (L.vsSize, L.fsSize, L.gen)))
        for k in L.vs: U.add_vs(k)
        for k in L.fs: U.add_fs(k)
        for v, f in L.prog: U.add_prog(v, f)
        for t in L.var: U.add_var(*t)
    U.dups = 0
    return U


def counts(L):
    return len(L.vs), len(L.fs), len(L.prog), len(L.var)


def fs_desc(k):
    # FsKey: numStages numTexGens texProjMask alphaTestOn alphaTest fog dstAlpha numIndStages ztex zfreeze pad[2]
    return 'stages=%d texgens=%d ind=%d alphaTest=%d fog=%d dstAlpha=%d ztex=%d' % (k[0], k[1], k[7], k[3], k[5], k[6], k[8])


def vs_desc(k):
    return 'texgens=%d chans=%d perVtxPos=%d' % (k[0], k[1], k[2])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('info'); s.add_argument('files', nargs='+')
    s = sub.add_parser('merge'); s.add_argument('-o', required=True); s.add_argument('--novariants', action='store_true'); s.add_argument('--maxvar', type=int); s.add_argument('files', nargs='+')
    s = sub.add_parser('diff'); s.add_argument('base'); s.add_argument('new')
    s = sub.add_parser('table'); s.add_argument('--base'); s.add_argument('files', nargs='+')
    s = sub.add_parser('decode'); s.add_argument('file')
    s = sub.add_parser('check'); s.add_argument('file')
    s = sub.add_parser('sub'); s.add_argument('-o', required=True); s.add_argument('--novariants', action='store_true'); s.add_argument('--maxvar', type=int); s.add_argument('file')
    s = sub.add_parser('split'); s.add_argument('--parts', type=int, required=True); s.add_argument('-o', required=True); s.add_argument('file')
    a = ap.parse_args()

    if a.cmd == 'split':
        L = read(a.file)
        n = len(L.prog)
        for part in range(a.parts):
            P = SList(); P.vsSize, P.fsSize, P.gen = L.vsSize, L.fsSize, L.gen
            lo, hi = part * n // a.parts, (part + 1) * n // a.parts
            chunk = L.prog[lo:hi]
            keys = set(chunk)
            for v, f in chunk:
                P.add_vs(v); P.add_fs(f); P.add_prog(v, f)
            for t in L.var:
                if (t[0], t[1]) in keys:
                    P.add_var(*t)
            P.dups = 0
            out = '%s%d.shl' % (a.o, part + 1)
            write(P, out)
            print('%s: vs %d fs %d prog %d var %d' % ((out,) + counts(P)))
        sys.exit(0)
    if a.cmd == 'info':
        for f in a.files:
            L = read(f)
            print('%-40s vs %4d fs %4d prog %4d var %5d  (%d bytes, gen %d, key sizes %d/%d)' % ((os.path.basename(f),) + counts(L) + (os.path.getsize(f), L.gen, L.vsSize, L.fsSize)))
    elif a.cmd == 'merge':
        U = merge([read(f) for f in a.files])
        write(U, a.o, a.novariants, a.maxvar)
        print('merged %d lists -> %s: vs %d fs %d prog %d var %d' % ((len(a.files), a.o) + counts(U)))
    elif a.cmd == 'sub':
        L = read(a.file)
        write(L, a.o, a.novariants, a.maxvar)
        print('%s -> %s' % (a.file, a.o))
    elif a.cmd == 'diff':
        B, N = read(a.base), read(a.new)
        nv = [k for k in N.vs if not B.has_vs(k)]
        nf = [k for k in N.fs if not B.has_fs(k)]
        npg = [t for t in N.prog if not B.has_prog(*t)]
        nva = [t for t in N.var if not B.has_var(*t)]
        print('new in %s over %s: vs %d fs %d prog %d var %d' % (a.new, a.base, len(nv), len(nf), len(npg), len(nva)))
        for k in nv: print('  VS', vs_desc(k))
        for k in nf: print('  FS', fs_desc(k))
    elif a.cmd == 'table':
        U = SList()
        if a.base:
            B = read(a.base); U = merge([B])
        print('%-34s %5s %5s %5s %6s | %4s %4s %5s %6s' % ('run', 'vs', 'fs', 'prog', 'var', '+vs', '+fs', '+prog', '+var'))
        for f in a.files:
            L = read(f)
            if U.vsSize is None:
                U.vsSize, U.fsSize, U.gen = L.vsSize, L.fsSize, L.gen
            nv = sum(1 for k in L.vs if U.add_vs(k))
            nf = sum(1 for k in L.fs if U.add_fs(k))
            npg = sum(1 for t in L.prog if U.add_prog(*t))
            nva = sum(1 for t in L.var if U.add_var(*t))
            print('%-34s %5d %5d %5d %6d | %4d %4d %5d %6d' % ((os.path.basename(f)[:34],) + counts(L) + (nv, nf, npg, nva)))
        print('%-34s %5d %5d %5d %6d' % (('union',) + counts(U)))
    elif a.cmd == 'decode':
        L = read(a.file)
        for i, k in enumerate(L.vs): print('VS %3d %s' % (i, vs_desc(k)))
        for i, k in enumerate(L.fs): print('FS %3d %s' % (i, fs_desc(k)))
    elif a.cmd == 'check':
        L = read(a.file)
        ok = L.dups == 0
        print('%s: vs %d fs %d prog %d var %d, duplicates %d -> %s' % ((a.file,) + counts(L) + (L.dups, 'OK' if ok else 'REWRITE WOULD HAPPEN')))
        sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
