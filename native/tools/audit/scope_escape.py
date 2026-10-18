#!/usr/bin/env python3
"""scope_escape.py <files...> : heuristic scan for the cPoseAccumulator::BlendTrans bug class: a pointer (or reference-capturing variable) declared in an outer block is
assigned the address of a local declared in an inner block (`p = &x;` / `p = arr;`) and the pointer is used again after the inner block closed (the local is dead:
clang may reuse / drop its stack slot).  Text based (the decomp is regularly formatted); reports candidates for manual review, no false-negative guarantee.
A second pattern: `const T& r = cond ? a : tmp_in_block` is not covered."""
import re, sys
DECL = re.compile(r'^\s*(?:static\s+)?(?:const\s+)?(?:unsigned\s+|signed\s+|struct\s+)?[A-Za-z_][\w:]*(?:<[^>;]*>)?\s*[\*&]*\s*([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:=[^;]*)?;\s*(?://.*)?$')
PTRDECL = re.compile(r'^\s*(?:const\s+)?[A-Za-z_][\w:]*(?:<[^>;]*>)?\s*\*+\s*(?:const\s+)?([A-Za-z_]\w*)\s*(?:=[^;]*)?;')
ASSIGN = re.compile(r'^\s*([A-Za-z_]\w*)\s*=\s*&\s*([A-Za-z_]\w*)\s*(?:[\.\[][^;]*)?;')
ASSIGN2 = re.compile(r'^\s*([A-Za-z_]\w*)\s*=\s*([A-Za-z_]\w*)\s*;')       # array decay: p = arr;
KW = {'return', 'else', 'delete', 'new', 'goto', 'case', 'break', 'continue', 'typedef', 'using', 'extern'}
def scan(path):
    out = []
    lines = open(path, errors='replace').read().split('\n')
    depth = 0; scopes = []              # stack of ids
    decl = {}                           # name -> list of (depth, line, scope_id, kind)
    sid = 0; stack = [0]; closed_at = {}   # scope id -> line where it closed
    var_scope = {}                      # (name) -> list of (scope_id, line, is_ptr)
    events = []                         # (line, type, data)
    for i, raw in enumerate(lines):
        l = re.sub(r'//.*$', '', raw)
        l = re.sub(r'"(\\.|[^"\\])*"', '""', l)
        s = l.strip()
        m = None
        if s and not s.startswith('#') and s.split()[0] not in KW and not s.startswith('}') :
            m = DECL.match(l)
            if m:
                nm = m.group(1)
                if nm not in KW:
                    var_scope.setdefault(nm, []).append((stack[-1], i, bool(PTRDECL.match(l))))
        a = ASSIGN.match(l) or ASSIGN2.match(l)
        if a:
            events.append((i, 'assign', (a.group(1), a.group(2), stack[-1], bool(ASSIGN.match(l)))))
        for ch in l:
            if ch == '{':
                sid += 1; stack.append(sid)
            elif ch == '}':
                if len(stack) > 1:
                    closed_at[stack.pop()] = i
    # second pass for uses: need scope membership of the lhs declaration; re-walk
    stack = [0]; sid = 0; parent = {0: None}; cur = [0]
    scope_of_line = []
    for i, raw in enumerate(lines):
        l = re.sub(r'//.*$', '', raw)
        scope_of_line.append(cur[-1])
        for ch in l:
            if ch == '{':
                sid += 1; parent[sid] = cur[-1]; cur.append(sid)
            elif ch == '}':
                if len(cur) > 1: cur.pop()
    def inside(s, anc):
        while s is not None:
            if s == anc: return True
            s = parent.get(s)
        return False
    for (i, t, d) in events:
        p, x, sc, amp = d
        # rhs declared in scope sc or an ancestor, lhs declared in a strict ancestor of the rhs scope?
        xd = [v for v in var_scope.get(x, []) if v[1] < i]
        pdcl = [v for v in var_scope.get(p, []) if v[1] < i]
        if not xd: continue
        xs = xd[-1]
        if pdcl: ps = pdcl[-1]
        else:                                          # parameter / member / global: outer to the whole function body
            fs = scope_of_line[i]
            while fs is not None and parent.get(fs) not in (None, 0): fs = parent.get(fs)
            ps = (fs, -1, False)
        if not (ps[0] != xs[0] and inside(xs[0], ps[0])): continue            # lhs scope is a strict ancestor of rhs scope
        if xs[0] == 0: continue
        if not amp and not re.search(r'\b%s\s*\[' % re.escape(x), lines[xs[1]]): continue            # p = arr only for arrays
        close = closed_at.get(xs[0])
        if close is None: continue
        use = None
        for j in range(close + 1, min(len(lines), close + 400)):
            if scope_of_line[j] is not None and not inside(scope_of_line[j], ps[0]): break
            if re.search(r'\b%s\b' % re.escape(p), lines[j]) and not re.match(r'^\s*%s\s*=[^=]' % re.escape(p), lines[j]):
                use = j; break
        if use is not None:
            out.append((path, i + 1, p, x, xs[1] + 1, close + 1, use + 1))
    return out
if __name__ == '__main__':
    n = 0
    for f in sys.argv[1:]:
        for (path, i, p, x, dl, cl, ul) in scan(f):
            print('%s:%d: %s = &%s (declared line %d, scope closes line %d) then %s used at line %d' % (path, i, p, x, dl, cl, p, ul)); n += 1
    print('candidates:', n, file=sys.stderr)
