#!/usr/bin/env python3
"""declscan.py: scans the game's headers and sources for function declarations: static members, return types and C prototypes.
   index = scan(root)   index.static  : set of 'Class::name'
                        index.ret     : dict 'Class::name' / 'name' -> list of return type texts
                        index.proto   : dict name -> list of parameter-list texts (free functions and members)"""
import re, os, collections

KEYW = {'return', 'else', 'new', 'delete', 'case', 'goto', 'throw', 'typedef', 'if', 'while', 'for', 'switch', 'do', 'sizeof', 'using', 'friend', 'operator'}


def strip(text):
    text = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), text, flags=re.S)
    text = re.sub(r'//[^\n]*', lambda m: ' ' * len(m.group(0)), text)
    text = re.sub(r'"(?:\\.|[^"\\\n])*"', lambda m: m.group(0) if m.group(0) == '"C"' else '""' + ' ' * (len(m.group(0)) - 2), text)
    return text


def class_spans(text):
    spans = []
    for m in re.finditer(r'\b(?:class|struct|union)\s+(\w+)\s*(?::[^{;]*)?\{', text):
        depth, i = 1, m.end()
        while i < len(text) and depth:
            c = text[i]
            if c == '{': depth += 1
            elif c == '}': depth -= 1
            i += 1
        spans.append((m.group(1), m.end(), i))
    return spans


DECL = re.compile(r'(?:^|[;{}\n])\s*((?:(?:static|inline|virtual|extern\s+"C"|const|volatile|explicit|__attribute__\(\([^)]*\)\))\s+)*)'
                  r'((?:unsigned\s+|signed\s+|const\s+|struct\s+|class\s+|enum\s+)*[A-Za-z_][\w:]*(?:<[^<>;{}]*>)?(?:\s*[\*&])*)\s+'
                  r'(?:(\w+(?:<[^<>;{}]*>)?)::)?(~?\w+)\s*\(([^()]*(?:\([^()]*\)[^()]*)*)\)\s*(const)?\s*(?:;|\{|=\s*0)')


def split_top(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch in '<([': depth += 1
        elif ch in '>)]': depth -= 1
        if ch == ',' and depth == 0: out.append(cur); cur = ''
        else: cur += ch
    if cur.strip(): out.append(cur)
    return out


class Index:
    def __init__(self):
        self.static = set()
        self.ret = collections.defaultdict(list)
        self.proto = collections.defaultdict(list)


def scan_text(text, idx, is_header):
    text = strip(text)
    spans = class_spans(text) if is_header else []
    for m in DECL.finditer(text):
        quals, ret, cls, name, params, const = m.groups()
        base = ret.strip()
        first = base.split()[0] if base.split() else ''
        if first in KEYW or name in KEYW or base in KEYW:
            continue
        pos = m.start(4)
        if cls is None and spans:
            inner = [s for s in spans if s[1] <= pos < s[2]]
            if inner:
                cls = min(inner, key=lambda s: s[2] - s[1])[0]
        key = (cls + '::' + name) if cls else name
        rt = re.sub(r'\s+', ' ', base)
        pt = params.strip()
        npar = 0 if pt in ('', 'void') else len(split_top(pt))
        idx.ret[key].append((rt, npar))
        if cls:
            idx.ret[name].append((rt, npar))
        idx.proto[name].append(params.strip())
        if cls and 'static' in quals:
            idx.static.add(cls + '::' + name)


def scan(root):
    idx = Index()
    for sub, exts, hdr in (('include', ('.h', '.inl', '.hpp'), True), ('libs', ('.h',), True), ('ref', ('.h',), True), ('src', ('.cpp',), False)):
        for dp, dn, fn in os.walk(os.path.join(root, sub)):
            for f in fn:
                if f.endswith(exts):
                    try:
                        scan_text(open(os.path.join(dp, f), errors='replace').read(), idx, hdr)
                    except Exception as e:
                        pass
    return idx


if __name__ == '__main__':
    import sys
    idx = scan(sys.argv[1])
    print(len(idx.static), 'static members;', len(idx.ret), 'return type keys;', len(idx.proto), 'prototypes')
    for k in sys.argv[2:]:
        print(k, 'ret', idx.ret.get(k), 'proto', idx.proto.get(k.split('::')[-1]), 'static', k in idx.static)
