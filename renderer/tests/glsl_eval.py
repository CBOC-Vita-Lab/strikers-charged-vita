#!/usr/bin/env python3
"""Evaluates the fragment shader text vgx_shadergen.cpp generates (a GLSL ES 1.00 subset) over many fragments at once, in float32 with numpy.
Used by run_tev_canon_test.sh: the same key's text with MSC_VGX_TEV_CANON off and on, the same random and adversarial inputs, compared in 8 bits.
Every value is an array (N, k), k = 1 for a float, 2..4 for vectors. Unknown syntax raises: a test must not pass because something was skipped."""
import re
import numpy as np

F = np.float32
_TOK = re.compile(r'\s*(?:(\d+\.\d*(?:[eE][-+]?\d+)?|\.\d+|\d+)|([A-Za-z_]\w*)|(&&|\|\||==|!=|<=|>=|\+=|-=|\*=|[-+*/<>!?:=(),.;{}\[\]]))')


def tokens(s):
    out = []
    pos = 0
    s = re.sub(r'//[^\n]*', '', s)
    s = re.sub(r'(?m)^\s*#.*$', '', s)
    while pos < len(s):
        if s[pos:].strip() == '':
            break
        m = _TOK.match(s, pos)
        if not m:
            raise SyntaxError('token at ' + s[pos:pos + 30])
        pos = m.end()
        if m.group(1) is not None:
            out.append(('n', m.group(1)))
        elif m.group(2) is not None:
            out.append(('i', m.group(2)))
        else:
            out.append(('p', m.group(3)))
    return out


class Env:
    def __init__(self, n, uniforms, tex, varyings, fragz):
        self.n = n
        self.v = {}
        self.u = uniforms      # name -> array (len, 4) for u_tev
        self.tex = tex         # callable(map, uvkey) -> (n,4)
        self.var = varyings    # name -> (n,3 or 4)
        self.fragz = fragz
        self.discard = np.zeros(n, bool)
        self.frag = None


SW = {'x': 0, 'y': 1, 'z': 2, 'w': 3, 'r': 0, 'g': 1, 'b': 2, 'a': 3, 's': 0, 't': 1, 'p': 2, 'q': 3}


def swz(a, name):
    idx = [SW[c] for c in name]
    return a[:, idx]


class Parser:
    def __init__(self, toks, env):
        self.t = toks
        self.i = 0
        self.e = env

    def peek(self):
        return self.t[self.i] if self.i < len(self.t) else ('e', '')

    def take(self, v=None):
        k = self.t[self.i]
        if v is not None and k[1] != v:
            raise SyntaxError('expected %s got %s at %d' % (v, k, self.i))
        self.i += 1
        return k

    def accept(self, v):
        if self.peek()[1] == v and self.peek()[0] == 'p':
            self.i += 1
            return True
        return False

    # ---- expressions (precedence climbing)
    def expr(self):
        return self.ternary()

    def ternary(self):
        c = self.oror()
        if self.accept('?'):
            a = self.expr()
            self.take(':')
            b = self.expr()
            return np.where(c.astype(bool), a, b)
        return c

    def oror(self):
        a = self.andand()
        while self.accept('||'):
            b = self.andand()
            a = (a.astype(bool) | b.astype(bool)).astype(F)
        return a

    def andand(self):
        a = self.eq()
        while self.accept('&&'):
            b = self.eq()
            a = (a.astype(bool) & b.astype(bool)).astype(F)
        return a

    def eq(self):
        a = self.rel()
        while self.peek()[1] in ('==', '!=') and self.peek()[0] == 'p':
            op = self.take()[1]
            b = self.rel()
            a = ((a == b) if op == '==' else (a != b)).astype(F)
        return a

    def rel(self):
        a = self.add()
        while self.peek()[1] in ('<', '>', '<=', '>=') and self.peek()[0] == 'p':
            op = self.take()[1]
            b = self.add()
            a = {'<': a < b, '>': a > b, '<=': a <= b, '>=': a >= b}[op].astype(F)
        return a

    def add(self):
        a = self.mul()
        while self.peek()[1] in ('+', '-') and self.peek()[0] == 'p':
            op = self.take()[1]
            b = self.mul()
            a = (a + b) if op == '+' else (a - b)
        return a

    def mul(self):
        a = self.unary()
        while self.peek()[1] in ('*', '/') and self.peek()[0] == 'p':
            op = self.take()[1]
            b = self.unary()
            with np.errstate(all='ignore'):
                a = (a * b) if op == '*' else (a / b)
        return a

    def unary(self):
        if self.accept('-'):
            return -self.unary()
        if self.accept('!'):
            return (~self.unary().astype(bool)).astype(F)
        return self.postfix()

    def postfix(self):
        a = self.primary()
        while True:
            if self.peek() == ('p', '.'):
                self.take()
                name = self.take()[1]
                a = swz(a, name)
            elif self.peek() == ('p', '['):
                raise SyntaxError('index on a value')
            else:
                return a

    def args(self):
        self.take('(')
        r = []
        if not self.accept(')'):
            while True:
                r.append(self.expr())
                if self.accept(')'):
                    break
                self.take(',')
        return r

    def primary(self):
        k, v = self.take()
        e = self.e
        if k == 'n':
            return np.full((e.n, 1), F(float(v)), F)
        if k == 'p' and v == '(':
            a = self.expr()
            self.take(')')
            return a
        if k != 'i':
            raise SyntaxError('primary %s' % v)
        if v in ('true', 'false'):
            return np.full((e.n, 1), F(v == 'true'), F)
        if v == 'gl_FragCoord':
            self.take('.')
            name = self.take()[1]
            if name == 'z':
                return e.fragz[:, None]
            if name == 'xy':
                return np.stack([np.full(e.n, F(100)), np.full(e.n, F(100))], 1).astype(F)
            raise SyntaxError('gl_FragCoord.' + name)
        if v == 'u_tev' and self.peek() == ('p', '['):
            self.take('[')
            idx = int(self.take()[1])
            self.take(']')
            return e.u[idx]
        if v in ('vec2', 'vec3', 'vec4', 'float', 'bool', 'int'):
            a = self.args()
            if v in ('float', 'bool', 'int'):
                return a[0].astype(F)
            n = int(v[3])
            parts = np.concatenate(a, 1)
            if parts.shape[1] == 1:
                parts = np.repeat(parts, n, 1)
            return parts[:, :n].astype(F)
        if v == 'texture2D':
            self.take('(')
            m = self.take()[1]
            self.take(',')
            s0 = self.i
            uv = self.expr()
            s1 = self.i
            self.take(')')
            return e.tex(m, ' '.join(t[1] for t in self.t[s0:s1]))
        if v in ('mix', 'clamp', 'min', 'max', 'floor', 'abs', 'step', 'dot', 'exp2', 'sqrt', 'fract'):
            a = self.args()
            with np.errstate(all='ignore'):
                if v == 'mix':
                    return (a[0] * (F(1) - a[2]) + a[1] * a[2]).astype(F)
                if v == 'clamp':
                    return np.minimum(np.maximum(a[0], a[1]), a[2]).astype(F)
                if v == 'min':
                    return np.minimum(a[0], a[1]).astype(F)
                if v == 'max':
                    return np.maximum(a[0], a[1]).astype(F)
                if v == 'floor':
                    return np.floor(a[0]).astype(F)
                if v == 'abs':
                    return np.abs(a[0]).astype(F)
                if v == 'step':
                    return (a[1] >= a[0]).astype(F)
                if v == 'dot':
                    return (a[0] * a[1]).sum(1, keepdims=True).astype(F)
                if v == 'exp2':
                    return np.exp2(a[0]).astype(F)
                if v == 'sqrt':
                    return np.sqrt(a[0]).astype(F)
                if v == 'fract':
                    return (a[0] - np.floor(a[0])).astype(F)
        if v in e.v:
            return e.v[v]
        if v in e.var:
            return e.var[v]
        raise NameError(v)

    # ---- statements
    def lvalue_assign(self, name, sw, op, val):
        e = self.e
        if sw is None:
            cur = e.v.get(name)
            if op == '=':
                if cur is not None and val.shape[1] == 1 and cur.shape[1] > 1:
                    val = np.repeat(val, cur.shape[1], 1)
                e.v[name] = val.astype(F)
            else:
                e.v[name] = ((cur + val) if op == '+=' else (cur - val) if op == '-=' else (cur * val)).astype(F)
            return
        idx = [SW[c] for c in sw]
        cur = e.v[name].copy()
        if op != '=':
            val = (cur[:, idx] + val) if op == '+=' else (cur[:, idx] - val) if op == '-=' else (cur[:, idx] * val)
        if val.shape[1] == 1 and len(idx) > 1:
            val = np.repeat(val, len(idx), 1)
        cur[:, idx] = val
        e.v[name] = cur

    def stmt(self):
        e = self.e
        k, v = self.peek()
        if v == '{' and k == 'p':
            self.take()
            saved = dict(e.v)
            while not self.accept('}'):
                self.stmt()
            # (GLSL scoping: names declared inside go; assignments to outer names stay)
            for n in list(e.v):
                if n not in saved:
                    del e.v[n]
            return
        if k == 'i' and v in ('vec2', 'vec3', 'vec4', 'float', 'bool', 'int'):
            self.take()
            ty = v
            while True:
                name = self.take()[1]
                if self.accept('='):
                    val = self.expr()
                    if ty != 'bool' and ty != 'float' and val.shape[1] == 1:
                        val = np.repeat(val, int(ty[3]), 1)
                    e.v[name] = val.astype(F)
                else:
                    e.v[name] = np.zeros((e.n, 1 if ty in ('float', 'bool', 'int') else int(ty[3])), F)
                if self.accept(';'):
                    break
                self.take(',')
            return
        if k == 'i' and v == 'if':
            self.take()
            self.take('(')
            c = self.expr()
            self.take(')')
            self.take('discard')
            self.take(';')
            e.discard |= ~c.astype(bool)[:, 0]
            return
        if k == 'i':
            name = self.take()[1]
            sw = None
            if self.accept('.'):
                sw = self.take()[1]
            op = self.take()[1]
            val = self.expr()
            self.take(';')
            if name == 'gl_FragColor':
                e.frag = val
                return
            self.lvalue_assign(name, sw, op, val)
            return
        raise SyntaxError('statement at %s' % (self.peek(),))


def evaluate(text, n, uniforms, tex, varyings, fragz):
    """-> (colour (n,4) float32 gl_FragColor, discard (n,) bool)."""
    body = text[text.index('void main()') + len('void main()'):]
    toks = tokens(body)
    env = Env(n, uniforms, tex, varyings, fragz)
    p = Parser(toks, env)
    p.take('{')
    while not (p.peek() == ('p', '}') and p.i == len(toks) - 1):
        p.stmt()
    return env.frag, env.discard
