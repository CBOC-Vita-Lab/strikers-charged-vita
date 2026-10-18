#!/usr/bin/env python3
"""mwcc.py: Metrowerks (CodeWarrior PPC EABI) C++ name demangling, the part dtlive needs: qualified name, method, const flag and the parameter
list as ABI kinds, plus the same ABI kinds for Itanium names demangled by c++filt (native side).
Kinds (one char per parameter, in declaration order):  p pointer/reference/function pointer,  i 32-bit integer class (int, short, char, bool, enum, long),
f float, d double, L 64-bit integer, S class passed by value (unsupported by the ABI shim, reported), . ellipsis."""
import re, subprocess

BASIC = {'i': 'i', 's': 'i', 'c': 'i', 'l': 'i', 'b': 'i', 'w': 'i', 'f': 'f', 'd': 'd', 'r': 'd', 'x': 'L', 'v': 'v', 'e': '.'}
STRUCT_BY_VALUE = set()          # class names known to be passed by value (set by gen from headers); everything else named is an enum -> i


class Fail(Exception):
    pass


def _ident(s, i):
    m = re.compile(r'\d+').match(s, i)
    if not m:
        raise Fail('ident at %d in %s' % (i, s))
    n = int(m.group(0)); j = m.end()
    if j + n > len(s):
        raise Fail('ident length')
    return s[j:j + n], j + n


def parse_qual(s, i):
    """class qualifier: <len>name | Q<n>(<len>name)+ ; returns (list_of_names, new_index)"""
    if s[i] == 'Q':
        m = re.compile(r'Q(\d)').match(s, i)
        n = int(m.group(1)); j = m.end(); names = []
        if j < len(s) and s[j] == '_':
            j += 1
        for _ in range(n):
            nm, j = _ident(s, j); names.append(nm)
        return names, j
    nm, j = _ident(s, i)
    return [nm], j


def parse_type(s, i, params):
    """one type; returns (kind, new_index). 'params' = list of kinds so far (for T<n> back references)."""
    c = s[i]
    if c in 'CV':                     # const / volatile
        return parse_type(s, i + 1, params)
    if c == 'U':                      # unsigned
        k, j = parse_type(s, i + 1, params); return k, j
    if c == 'S' and i + 1 < len(s) and s[i + 1] in 'cislb':   # signed char etc
        return 'i', i + 2
    if c in 'PR':
        # pointer / reference to ...; consume the pointee fully
        j = i + 1
        k, j = parse_type(s, j, params)
        return 'p', j
    if c == 'F':                      # function type: F<params>_<ret>
        j = i + 1; sub = []
        while j < len(s) and s[j] != '_':
            k, j = parse_type(s, j, sub); sub.append(k)
        if j >= len(s):
            raise Fail('function type end')
        j += 1
        k, j = parse_type(s, j, sub)
        return 'F', j
    if c == 'A':                      # array A<n>_T  (decays to pointer when a parameter)
        m = re.compile(r'A(\d*)_').match(s, i)
        if not m:
            raise Fail('array')
        k, j = parse_type(s, m.end(), params)
        return 'p', j
    if c == 'M':                      # pointer to member: M<class><type>
        names, j = parse_qual(s, i + 1)
        k, j = parse_type(s, j, params)
        return 'p', j
    if c == 'T':                      # back reference T<n>
        m = re.compile(r'T(\d)').match(s, i)
        n = int(m.group(1))
        if n < 1 or n > len(params):
            raise Fail('T ref')
        return params[n - 1], m.end()
    if c in BASIC:
        return BASIC[c], i + 1
    if c.isdigit() or c == 'Q':
        names, j = parse_qual(s, i)
        nm = '::'.join(names)
        # template arguments inside names are part of the length-counted identifier
        return ('S' if nm in STRUCT_BY_VALUE else 'i'), j
    raise Fail('type char %r at %d in %s' % (c, i, s))


def parse_params(s, i, texts=None):
    params = []
    if texts is None:
        texts = []
    while i < len(s) and s[i] != '_':
        if s[i] == 'N':                   # N<count><index>: repeat param <index> <count> times
            m = re.compile(r'N(\d)(\d)').match(s, i)
            if not m:
                raise Fail('N')
            for _ in range(int(m.group(1))):
                params.append(params[int(m.group(2)) - 1]); texts.append(texts[int(m.group(2)) - 1])
            i = m.end(); continue
        j0 = i
        k, i = parse_type(s, i, params)
        params.append(k); texts.append(s[j0:i])
    return params, i


def demangle(sym):
    """-> dict(cls=[names], name=str, const=bool, params=[kinds]) or None (not a function symbol we can parse)"""
    if '__' not in sym[1:]:
        return None
    pos = 1
    while True:
        p = sym.find('__', pos)
        if p < 0:
            return None
        pos = p + 1
        # templates in the function name: skip '__' inside <...>
        if sym[:p].count('<') != sym[:p].count('>'):
            continue
        rest = sym[p + 2:]
        try:
            i = 0; cls = []
            if rest[:1] in ('F',) or rest[:2] == 'CF':
                pass
            elif rest[:1].isdigit() or rest[:1] == 'Q':
                cls, i = parse_qual(rest, 0)
            else:
                continue
            const = False
            if rest[i:i + 1] == 'C':
                const = True; i += 1
            if rest[i:i + 1] == 'S':      # static member marker? (not seen) keep strict
                continue
            if rest[i:i + 1] != 'F':
                continue
            texts = []
            params, j = parse_params(rest, i + 1, texts)
            if j != len(rest) and not (rest[j:j + 1] == '_'):
                continue
            if params == ['v']:
                params = []; texts = []
            return dict(cls=cls, name=sym[:p], const=const, params=params, ptypes=texts)
        except (Fail, IndexError):
            continue


# ---- native (Itanium, demangled by c++filt) ---------------------------------------------------------------
def split_top(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch in '<([':
            depth += 1
        elif ch in '>)]':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip()); cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def native_kind(t):
    t = t.strip()
    if '(*)' in t or t.endswith('*') or t.endswith('&') or t.endswith('&&') or re.search(r'\*\s*const$', t) or '[' in t:
        return 'p'
    t = re.sub(r'\bconst\b|\bvolatile\b', '', t).strip()
    if t in ('float', 'f32'): return 'f'
    if t in ('double', 'f64'): return 'd'
    if t in ('long long', 'unsigned long long', '__int64', 'unsigned __int64', 's64', 'u64'): return 'L'
    if t in ('int', 'unsigned int', 'short', 'unsigned short', 'char', 'unsigned char', 'signed char', 'bool', 'long', 'unsigned long', 'wchar_t'): return 'i'
    if t == '...': return '.'
    if t == 'void': return 'v'
    return 'S' if t in STRUCT_BY_VALUE else 'i'


def native_parse(dem):
    """demangled text 'ns::Class::name(args) const' -> (qualified, params kinds) or None"""
    m = re.match(r'^(.*?)\((.*)\)\s*(const)?\s*$', dem)
    if not m:
        return None
    head = m.group(1)
    # templates in head may contain '(' (rare): require balanced <>
    if head.count('<') != head.count('>'):
        return None
    params = [native_kind(t) for t in split_top(m.group(2))]
    if params == ['v']:
        params = []
    return head.strip(), params


def demangle_native(symbols):
    """batch c++filt"""
    if not symbols:
        return {}
    out = subprocess.run(['c++filt'], input='\n'.join(symbols) + '\n', capture_output=True, text=True).stdout.split('\n')
    return dict(zip(symbols, out))


if __name__ == '__main__':
    import sys
    for a in sys.argv[1:]:
        print(a, demangle(a))


def mwcc_pointee_names(ptypes):
    """['RC9nlVector3', 'f', 'Us'] -> ['nlVector3', None, None] (class / builtin name behind P R C V, None for non-pointers)"""
    out = []
    for t in ptypes:
        j = 0
        while j < len(t) and t[j] in 'PRCV':
            j += 1
        if j == 0 or not (t[0] in 'PR'):
            out.append(None); continue
        rest = t[j:]
        if rest[:1] in ('F', 'A'):                  # pointer to function / array: the native side reports no pointee name
            out.append(None); continue
        while rest[:1] in ('P', 'R', 'C', 'V'):     # pointer to pointer: the native side strips every '*'
            rest = rest[1:]
        mm = re.match(r'^(\d+)(.*)$', rest)
        if mm and len(mm.group(2)) >= int(mm.group(1)):
            out.append(mm.group(2)[:int(mm.group(1))])
        else:
            out.append({'f': 'float', 'i': 'int', 'b': 'bool', 'c': 'char', 's': 'short', 'Ui': 'unsigned int', 'Us': 'unsigned short', 'Uc': 'unsigned char', 'v': 'void'}.get(rest, rest))
    return out


def native_pointee_names(dem):
    """names of the pointee types of the pointer / reference parameters of a demangled C++ signature (None for the others)"""
    m = re.match(r'^(.*?)\((.*)\)\s*(const)?\s*$', dem)
    if not m:
        return None
    out = []
    for t in split_top(m.group(2)):
        t = t.strip()
        if t == 'void':
            continue
        if native_kind(t) == 'p' and '(*)' not in t:
            b = re.sub(r'\bconst\b|\bvolatile\b|[*&]', '', t)
            out.append(re.sub(r'\s+', ' ', b).strip())
        else:
            out.append(None)
    return out
