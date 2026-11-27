#!/usr/bin/env python3
"""printf-family argument-count checker for the project's own log wrappers (the ones the compiler cannot check because they
carry no __attribute__((format)); tools/check_printf_clang.py covers the ones it can).

For every call whose name looks like a printf/log helper, the first string literal among the top-level arguments is taken as
the format; conversions (%% and %n$ aside, '*' counts one argument) are compared with the arguments after it. Reports:
  COUNT   fewer or more arguments than conversions
  LL      a %ll conversion whose argument text shows no 64-bit cast or literal (a 32-bit value read as 64 bits on the Vita)
  SIZE    %zu/%zd/%zx/%j/%t/%hh (the Vita's C library does not know them)
Usage: python3 tools/check_printf_args.py [root] [--all]   exit 1 when a COUNT or SIZE finding exists (LL is listed for review).
"""
import os, re, sys

NAME = re.compile(r"\b((?:\w+::)*\w*(?:printf|Printf|PRINTF|Log|LOG|log|Report|Panic|Say|Warn|Err|Error|Note|Print|print|SetLine|EventF|Reply|WriteState|EndReason)\w*)\s*\(")
SKIP_DIRS = {"tests", "test", "tools", "ref", "third_party", "node_modules", "dependencies", ".git"}
CONV = re.compile(r"%(?:\d+\$)?[-+ #0]*(\*|\d+)?(?:\.(\*|\d+))?(hh|h|ll|l|L|q|z|j|t)?([diouxXeEfFgGaAcspn%])")
LL64 = re.compile(r"\(\s*(?:unsigned\s+)?(?:long\s+long|u?int64_t|uint64_t|U64|u64|s64)\s*\)|static_cast<\s*(?:unsigned\s+)?(?:long\s+long|u?int64_t)\s*>|U?LL\b|\bu64\b|\bint64_t\b|\buint64_t\b|\bPRI[ux]64\b|\bmicros\b|NowUs|NowNs|\bns\b")

def strip(text):
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1]); i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i); j = n if j < 0 else j
            out.append(" " * (j - i)); i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2); j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j])); i = j
        else:
            out.append(c); i += 1
    return "".join(out)

def split_args(text, i):
    """text[i] is just past '('. Returns (list of arg strings, index past ')')."""
    args, depth, cur, n = [], 1, [], len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            cur.append(text[i:j + 1]); i = j + 1; continue
        if c in "([{": depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                a = "".join(cur).strip()
                if a or args: args.append(a)
                return args, i + 1
        elif c == "," and depth == 1:
            args.append("".join(cur).strip()); cur = []; i += 1; continue
        elif c == ";" and depth == 1:
            return None, i
        cur.append(c); i += 1
    return None, n

def literal(arg):
    """Concatenated adjacent string literals -> body text, or None."""
    parts = re.findall(r'"((?:[^"\\\n]|\\.)*)"', arg)
    rest = re.sub(r'"(?:[^"\\\n]|\\.)*"', "", arg)
    if not parts or rest.strip(): return None
    return "".join(parts)

def main():
    root = next((a for a in sys.argv[1:] if not a.startswith("--")), os.getcwd())
    findings = []
    for base in ("native", "renderer"):
        for dp, dns, fns in os.walk(os.path.join(root, base)):
            dns[:] = [d for d in dns if d not in SKIP_DIRS]
            for fn in fns:
                if not fn.endswith((".cpp", ".c", ".h", ".hpp", ".inc")): continue
                p = os.path.join(dp, fn); rel = os.path.relpath(p, root)
                try: raw = open(p, encoding="utf-8", errors="replace").read()
                except OSError: continue
                if "%" not in raw: continue
                text = strip(raw)
                for m in NAME.finditer(text):
                    name = m.group(1)
                    if name in ("if", "while", "switch", "for", "return", "sizeof"): continue
                    args, end = split_args(text, m.end())
                    if not args: continue
                    k = next((j for j, a in enumerate(args) if literal(a) is not None), None)
                    if k is None or k > 3: continue
                    fmt = literal(args[k])
                    if "%" not in fmt: continue
                    rest = args[k + 1:]
                    if any("__VA_ARGS__" in a or a == "..." for a in args): continue
                    line = text.count("\n", 0, m.start()) + 1
                    need, convs = 0, []
                    for c in CONV.finditer(fmt):
                        if c.group(4) == "%": continue
                        for star in (c.group(1), c.group(2)):
                            if star == "*": need += 1
                        need += 1; convs.append(c)
                    if len(rest) != need:
                        findings.append(("COUNT", rel, line, f"{name}: {need} conversions, {len(rest)} arguments: {fmt[:70]!r}"))
                        continue
                    ai = 0
                    for c in convs:
                        ai += (c.group(1) == "*") + (c.group(2) == "*")
                        arg = rest[ai] if ai < len(rest) else ""
                        ai += 1
                        mod, ty = c.group(3) or "", c.group(4)
                        if mod in ("z", "j", "t", "hh") and ty in "diouxX":
                            findings.append(("SIZE", rel, line, f"{name}: %{mod}{ty} unsupported by the Vita C library: {fmt[:60]!r}"))
                        if mod in ("ll", "q") and ty in "diouxX" and "printf" not in name.lower() and not LL64.search(arg):
                            findings.append(("LL", rel, line, f"{name}: %ll{ty} argument {arg[:60]!r} shows no 64-bit type: {fmt[:50]!r}"))
    show_all = "--all" in sys.argv
    bad = 0
    for kind, rel, line, msg in sorted(findings):
        if kind == "LL" and not show_all and "--ll" not in sys.argv: continue
        print(f"{kind} {rel}:{line}: {msg}")
        bad += kind in ("COUNT", "SIZE")
    print(f"{len(findings)} findings, {bad} COUNT/SIZE")
    sys.exit(1 if bad else 0)

main()
