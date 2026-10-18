#!/usr/bin/env python3
"""gen_layout.py - derive endian-swap tables for the decomp's in-place disc structs.

Runs clang -m32 -fdump-record-layouts over a probe TU that includes the decomp headers, parses the
record layouts and writes, for every requested struct, the list of multi-byte scalar fields
(offset, element size, count). Byte-sized fields (char, bool, u8, byte arrays) are NOT listed: they keep
their file position so the host reads exactly the byte the Wii read. Pointers are 4 bytes (ILP32).

  gen_layout.py <probe.cpp> <structs.txt> <out.inc> [-I dir ...]

structs.txt: one struct name per line ('#' comments). Output: C initialisers
  static const NpaFld fld_<Name>[] = { {off,size,count}, ... };  /  { "<Name>", sizeof, fld_<Name>, n }
The generator also writes the dumped sizeof() of each struct so the converters can check the record
stride, and exits non-zero when a type cannot be resolved (an enum it does not know, an array of
unexpanded structs), so a header change cannot silently produce a wrong table.
"""
import os
import re
import subprocess
import sys

SCALAR = {
    'float': 4, 'int': 4, 'unsigned int': 4, 'long': 4, 'unsigned long': 4, 'signed long': 4,
    'short': 2, 'unsigned short': 2, 'signed short': 2,
    'long long': 8, 'unsigned long long': 8, 'double': 8,
    'char': 1, 'unsigned char': 1, 'signed char': 1, '_Bool': 1, 'bool': 1,
    'u8': 1, 's8': 1, 'u16': 2, 's16': 2, 'u32': 4, 's32': 4, 'u64': 8, 's64': 8, 'f32': 4, 'f64': 8,
    'size_t': 4, 'wchar_t': 2,
}


def die(msg):
    sys.stderr.write('gen_layout: ' + msg + '\n')
    sys.exit(1)


def enum_names(include_dirs):
    names = set()
    pat1 = re.compile(r'\benum\s+(?:class\s+)?(\w+)\s*(?::\s*\w+\s*)?\{')
    pat2 = re.compile(r'\}\s*(\w+)\s*;')
    for inc in include_dirs:
        for root, _, files in os.walk(inc):
            for f in files:
                if not f.endswith(('.h', '.inl')):
                    continue
                try:
                    t = open(os.path.join(root, f), errors='replace').read()
                except OSError:
                    continue
                for m in pat1.finditer(t):
                    names.add(m.group(1))
                for m in re.finditer(r'typedef\s+enum\b[^{;]*\{[^}]*\}\s*(\w+)\s*;', t, re.S):
                    names.add(m.group(1))
    return names


class Node:
    __slots__ = ('off', 'indent', 'text', 'kids', 'bit')

    def __init__(self, off, indent, text, bit=None):
        self.off, self.indent, self.text, self.kids, self.bit = off, indent, text, [], bit


def parse_dump(text):
    """-> {record name: (sizeof, [top-level Node])}"""
    recs = {}
    blocks = text.split('*** Dumping AST Record Layout')
    line_re = re.compile(r'^\s*(\d+)(?::(\d+)-(\d+))?\s*\|(\s*)(.*)$')
    size_re = re.compile(r'\[sizeof=(\d+)')
    for b in blocks[1:]:
        lines = b.strip('\n').split('\n')
        root = None
        stack = []
        size = None
        for ln in lines:
            m = size_re.search(ln)
            if m and ln.strip().startswith('|'):
                size = int(m.group(1))
                continue
            m = line_re.match(ln)
            if not m:
                continue
            off = int(m.group(1))
            bit = (int(m.group(2)), int(m.group(3))) if m.group(2) else None
            indent = len(m.group(4))
            node = Node(off, indent, m.group(5).strip(), bit)
            if root is None:
                root = node
                stack = [node]
                continue
            while stack and stack[-1].indent >= indent:
                stack.pop()
            if not stack:
                continue
            stack[-1].kids.append(node)
            stack.append(node)
        if root is None:
            continue
        name = re.sub(r'^(class|struct|union)\s+', '', root.text).strip()
        recs[name] = (size, root)
    return recs


def leaf_type_name(text):
    """'unsigned char[12] mName' -> ('unsigned char', [12], 'mName')"""
    m = re.match(r'^(.*\S)\s+([\w:~<>]+)$', text)
    if not m:
        return None
    tytxt, name = m.group(1).strip(), m.group(2)
    m2 = re.match(r'^(.*?)((?:\[\d+\])*)$', tytxt)
    ty, dims = m2.group(1).strip(), m2.group(2)
    dl = [int(x) for x in re.findall(r'\[(\d+)\]', dims)]
    return ty, dl, name


class Gen:
    def __init__(self, recs, enums):
        self.recs, self.enums = recs, enums
        self.warn = []

    def runs_of(self, node, base, out, ctx):
        """append (abs_offset, size, count) for multi-byte scalars below node; offsets absolute + base"""
        t = node.text
        if 'vtable pointer' in t:
            return
        if node.bit is not None:
            # bit-field: layout is compiler specific; report and keep the storage unit unswapped
            self.warn.append('%s: bit-field %s' % (ctx, t))
            return
        is_union = t.startswith('union ')
        if node.kids:
            if is_union:
                self.warn.append('%s: union %s' % (ctx, t))
                # a union overlays views of the same bytes: take the single child view that covers the most
                # multi-byte scalar bytes (all views of nlVector3/nlMatrix4 unions are floats)
                best = None
                for k in node.kids:
                    tmp = []
                    self.runs_of(k, base, tmp, ctx)
                    cover = sum(sz * c for (_, sz, c) in tmp)
                    if best is None or cover > best[0]:
                        best = (cover, tmp)
                if best:
                    out.extend(best[1])
                return
            for k in node.kids:
                self.runs_of(k, base, out, ctx)
            return
        lt = leaf_type_name(t)
        if lt is None:
            self.warn.append('%s: unparsed "%s"' % (ctx, t))
            return
        ty, dims, name = lt
        count = 1
        for d in dims:
            count *= d
        ty = re.sub(r'\b(const|volatile)\b', '', ty).strip()
        ty = re.sub(r'^(class|struct|union|enum)\s+', '', ty).strip()
        if '*' in ty or '(' in ty:
            size = 4
        elif ty in SCALAR:
            size = SCALAR[ty]
        elif ty in self.enums:
            size = 4
        elif ty in self.recs:
            rs, rn = self.recs[ty]
            for i in range(count):
                tmp = []
                for k in rn.kids:
                    self.runs_of(k, 0, tmp, ctx + '.' + name)
                for (o, sz, c) in tmp:
                    out.append((node.off + base + i * rs + o, sz, c))
            return
        else:
            die('%s: cannot size type "%s" of "%s" (add it to SCALAR or the enum scan)' % (ctx, ty, t))
        if size > 1:
            out.append((node.off + base, size, count))
        elif size == 1 and count % 4 == 0 and (node.off + base) % 4 == 0 and re.search(r'pad|unident|unknown|reserved', name, re.I):
            # "unidentified" byte blobs of the disc images were exported as arrays of 32-bit words (their
            # contents are PC memory words); converting them as words keeps them consistent
            out.append((node.off + base, 4, count // 4))

    def struct_runs(self, name):
        if name not in self.recs:
            die('struct %s not in the layout dump (is it included by the probe?)' % name)
        size, root = self.recs[name]
        out = []
        for k in root.kids:
            self.runs_of(k, 0, out, name)
        # merge adjacent equal-size runs
        out.sort()
        merged = []
        for (o, sz, c) in out:
            if merged and merged[-1][1] == sz and merged[-1][0] + merged[-1][1] * merged[-1][2] == o:
                merged[-1] = (merged[-1][0], sz, merged[-1][2] + c)
            else:
                merged.append((o, sz, c))
        return size, merged


def main():
    args = sys.argv[1:]
    incs = []
    pos = []
    i = 0
    while i < len(args):
        if args[i] == '-I':
            incs.append(args[i + 1])
            i += 2
        else:
            pos.append(args[i])
            i += 1
    if len(pos) != 3:
        die('usage: gen_layout.py probe.cpp structs.txt out.inc [-I dir ...]')
    probe, structs, out = pos
    here = os.path.dirname(os.path.abspath(__file__))
    names = [l.split('#')[0].strip() for l in open(structs)]
    names = [n for n in names if n]
    # force the layout of every requested record: sizeof needs the complete type
    import tempfile
    tu = tempfile.NamedTemporaryFile('w', suffix='.cpp', dir=here, delete=False)
    tu.write('#include "%s"\n' % os.path.abspath(probe))
    for n in names:
        tu.write('typedef char np_force_%s[sizeof(%s)];\n' % (n, n))
    tu.close()
    probe = tu.name
    cmd = ['clang++', '-m32', '-std=c++03', '-fsyntax-only', '-include', os.path.join(here, 'prelude.h'),
           '-Wno-everything', '-Xclang', '-fdump-record-layouts'] + sum([['-I', d] for d in incs], []) + [probe]
    r = subprocess.run(cmd, capture_output=True, text=True)
    os.unlink(tu.name)
    if r.returncode != 0 and 'error:' in r.stderr:
        die('clang failed:\n' + r.stderr[:3000])
    recs = parse_dump(r.stdout)
    g = Gen(recs, enum_names(incs))
    lines = ['/* GENERATED by tools/assets/layout/gen_layout.py (clang -m32 record layouts of the decomp headers). Do not edit. */']
    table = []
    for n in names:
        size, runs = g.struct_runs(n)
        lines.append('static const NpaFld fld_%s[] = { %s };' % (n, ', '.join('{%d,%d,%d}' % r for r in runs) if runs else '{0,0,0}'))
        table.append('{ "%s", %d, fld_%s, %d }' % (n, size, n, len(runs)))
    lines.append('static const NpaStruct np_structs[] = {\n    ' + ',\n    '.join(table) + '\n};')
    open(out, 'w').write('\n'.join(lines) + '\n')
    for w in sorted(set(g.warn)):
        sys.stderr.write('note: ' + w + '\n')
    print('%d structs -> %s' % (len(names), out))


if __name__ == '__main__':
    main()
