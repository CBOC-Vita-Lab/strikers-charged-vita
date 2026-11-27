#!/usr/bin/env python3
"""vita_hot_order.py: hot-code order for the Vita link (make variable NP_ORDER; native/platform/vita/Makefile.vpk).

  make   --profdata P --map REF.map --out order.ld [--cover 0.99] [--llvm-profdata llvm-profdata-18]
         Function weights come from an llvm profile (sum of the block counters of each function, hottest first).
         Section names come from the map of a reference link of the same sources (clang ThinLTO + PGO name the sections
         .text.hot.<fn>, .text.<fn>, .text.unlikely.<fn>; a promoted static has a ".llvm.<hash>" tail that is not stable, so
         it is matched by a glob). The output is a GNU ld --section-ordering-file (binutils >= 2.43: ".text : { *(sec) ... }").
  check  --order order.ld --map NEW.map [--elf NEW.elf --nm arm-vita-eabi-nm]
         ld IGNORES a name that matches nothing (tested), so this is the report: statements that matched no input section, and
         whether the matched sections sit first, in listed order, at the start of the .text output section. Exit 1 on any problem.
  pages  --map X.map --profdata P [--order order.ld] [--llvm-profdata ...]
         Locality figure: how many 4 KiB / 64 KiB pages hold the functions that carry 90 % of the profile weight.
  syms   --elf X.elf [--nm ...]: one sorted "name size" line per function symbol (to compare two links: same set, nothing lost).
"""
import argparse, re, subprocess, sys, collections

SEC = re.compile(r'^ (\.text(?:\.hot|\.unlikely|\.startup|\.exit)?\.\S+)(?:\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+))?\s*$')
CONT = re.compile(r'^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)\s*$')
LLVM = re.compile(r'\.llvm\.\d+$')


def read_text_sections(mapfile):
    """[(section name, addr, size, file)] in output order, from the main .text output section of a GNU ld map."""
    out, inside, pend = [], False, None
    for line in open(mapfile, errors='replace'):
        if not inside:
            if re.match(r'^\.text\s+0x[0-9a-f]+\s+0x[0-9a-f]+', line):
                inside = True
            continue
        if line[:1] not in ' \n' and line[:1] != '*':
            if re.match(r'^\.\w', line):
                break
        m = SEC.match(line)
        if m:
            if m.group(2):
                out.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4)))
                pend = None
            else:
                pend = m.group(1)
            continue
        if pend:
            c = CONT.match(line)
            if c:
                out.append((pend, int(c.group(1), 16), int(c.group(2), 16), c.group(3)))
            pend = None
    return [s for s in out if s[2] > 0]


def fn_of_section(name):
    n = re.sub(r'^\.text(?:\.hot|\.unlikely|\.startup|\.exit)?\.', '', name)
    return LLVM.sub('', n)


def read_profile(path, tool):
    """{function name: weight}; static functions are 'file.cpp:name' in the profile, the section carries only name."""
    txt = subprocess.run([tool, 'show', '--all-functions', '--counts', path], check=True, capture_output=True, text=True).stdout
    w, cur = {}, None
    for line in txt.splitlines():
        if line.startswith('  ') and not line.startswith('    ') and line.rstrip().endswith(':'):
            cur = line.strip()[:-1]
            cur = cur.split(':', 1)[1] if (':' in cur and not cur.startswith('_Z')) else cur
        elif line.strip().startswith('Block counts:') and cur is not None:
            c = [int(x) for x in re.findall(r'\d+', line.split(':', 1)[1])]
            w[cur] = w.get(cur, 0) + sum(c)
    return w


def cmd_make(a):
    prof = read_profile(a.profdata, a.llvm_profdata)
    secs = read_text_sections(a.map)
    by_fn = collections.defaultdict(list)
    for s in secs:
        if s[0].startswith('.text.unlikely.') or s[0].startswith('.text.exit.'):
            continue
        by_fn[fn_of_section(s[0])].append(s[0])
    ranked = sorted((f for f, w in prof.items() if w > 0), key=lambda f: -prof[f])
    total = sum(prof[f] for f in ranked if f in by_fn) or 1
    lines, seen, acc, hit, miss = [], set(), 0, 0, 0
    for f in ranked:
        if f not in by_fn:
            miss += 1
            continue
        for s in by_fn[f]:
            if s in seen:
                continue
            seen.add(s)
            # exact name; a promoted static (".llvm.<hash>") by glob, so the hash may change between links
            lines.append('*(%s)' % (LLVM.sub('.llvm.*', s)))
        hit += 1
        acc += prof[f]
        if acc >= a.cover * total:
            break
    with open(a.out, 'w') as o:
        o.write('/* vita_hot_order.py: %d functions (%d statements) of %s, hottest first; weight = sum of block counters, cover %.3f */\n'
                % (hit, len(lines), a.profdata, a.cover))
        o.write('.text : {\n')
        for l in lines:
            o.write(' %s\n' % l)
        o.write('}\n')
    print('order file %s: %d functions / %d statements; profile functions with weight>0 %d, absent from the map %d' % (a.out, hit, len(lines), len(ranked), miss))


def cmd_check(a):
    stm = [m.group(1) for m in re.finditer(r'\*\((\S+?)\)', open(a.order).read().split('{', 1)[1])]
    secs = read_text_sections(a.map)
    names = [s[0] for s in secs]
    ok = True
    matched_idx = []
    pats = [re.compile('^' + re.escape(p).replace(r'\*', '.*') + '$') for p in stm]
    first = {}
    for i, n in enumerate(names):
        for j, r in enumerate(pats):
            if j not in first and r.match(n):
                first[j] = i
    unmatched = [stm[j] for j in range(len(stm)) if j not in first]
    if unmatched:
        ok = False
        print('NOT MATCHED by any input section (ld ignores these silently): %d of %d' % (len(unmatched), len(stm)))
        for u in unmatched[:20]:
            print('   ', u)
    # the matched sections must be the first ones of .text, in listed order
    order = [first[j] for j in range(len(stm)) if j in first]
    inversions = sum(1 for x, y in zip(order, order[1:]) if y < x)
    span_end = max(order) if order else 0
    n_matched = len(order)
    print('matched %d of %d statements; first-listed section index range %d..%d of %d; order inversions %d'
          % (n_matched, len(stm), min(order) if order else -1, span_end, len(names), inversions))
    if order and span_end > n_matched * 1.02 + 16:
        print('NOT CONTIGUOUS: the listed sections span %d section slots for %d statements' % (span_end + 1, n_matched))
        ok = False
    if inversions:
        ok = False
    lo = secs[min(order)][1] if order else 0
    hi = secs[span_end][1] + secs[span_end][2] if order else 0
    print('hot block: 0x%x..0x%x = %d bytes' % (lo, hi, hi - lo))
    print('OK' if ok else 'FAIL')
    return 0 if ok else 1


def simulate_order(secs, order_file):
    """Layout after a link with the order file: the matched sections first (listed order), then the others in their old order (sizes only; alignment ignored)."""
    stm = [m.group(1) for m in re.finditer(r'\*\((\S+?)\)', open(order_file).read().split('{', 1)[1])]
    pats = [re.compile('^' + re.escape(p).replace(r'\*', '.*') + '$') for p in stm]
    taken, first = set(), []
    for r in pats:
        for i, s in enumerate(secs):
            if i not in taken and r.match(s[0]):
                taken.add(i); first.append(i)
    seq = [secs[i] for i in first] + [s for i, s in enumerate(secs) if i not in taken]
    out, addr = [], secs[0][1]
    for n, _, size, f in seq:
        out.append((n, addr, size, f)); addr += size
    return out


def pages_of(sel):
    p4, p64 = set(), set()
    for addr, size in sel:
        p4.update(range(addr >> 12, ((addr + size - 1) >> 12) + 1))
        p64.update(range(addr >> 16, ((addr + size - 1) >> 16) + 1))
    return len(p4), len(p64)


def cmd_pages(a):
    """Pages holding hot sets, from the map; with --order also from the layout the order file would give (host-side simulation). Sets: the functions that
    carry 50/90/99/99.9 % of the block-counter weight (a few tight loops dominate it, so the low covers are tiny) and every function that ran (weight>0)."""
    prof = read_profile(a.profdata, a.llvm_profdata)
    secs = read_text_sections(a.map)
    layouts = [('now', secs)]
    if a.order:
        layouts.append(('ordered', simulate_order(secs, a.order)))
    for label, lay in layouts:
        ws = []
        for n, addr, size, f in lay:
            w = prof.get(fn_of_section(n), 0)
            if w > 0:
                ws.append((w, addr, size))
        ws.sort(reverse=True)
        total = sum(w for w, _, _ in ws) or 1
        res = []
        for cover in (0.5, 0.9, 0.99, 0.999):
            acc, sel = 0, []
            for w, addr, size in ws:
                sel.append((addr, size)); acc += w
                if acc >= cover * total:
                    break
            res.append('%g%%: %d fn %d B -> %d x4K %d x64K' % (cover * 100, len(sel), sum(s for _, s in sel), *pages_of(sel)))
        allsel = [(ad, sz) for _, ad, sz in ws]
        lo = min(x for x, _ in allsel); hi = max(x + s for x, s in allsel)
        print('[%s] %d sections; ran (weight>0) %d fn %d B -> %d x4K %d x64K, span %d KiB; cover ' % (label, len(lay), len(ws), sum(s for _, s in allsel), *pages_of(allsel), (hi - lo) // 1024) + ' | '.join(res))


def cmd_syms(a):
    txt = subprocess.run([a.nm, '-S', '--defined-only', '-n', a.elf], check=True, capture_output=True, text=True).stdout
    out = set()
    for l in txt.splitlines():
        p = l.split()
        if len(p) == 4 and p[2] in 'tTwW':
            out.add((p[3], int(p[1], 16)))
    for n, s in sorted(out):
        print(n, s)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest='cmd', required=True)
    for name in ('make', 'check', 'pages', 'syms'):
        p = sp.add_parser(name)
        p.add_argument('--profdata'); p.add_argument('--map'); p.add_argument('--out'); p.add_argument('--order'); p.add_argument('--elf')
        p.add_argument('--cover', type=float, default=0.999 if name == 'make' else 0.9)
        p.add_argument('--llvm-profdata', default='llvm-profdata-18'); p.add_argument('--nm', default='arm-vita-eabi-nm')
    a = ap.parse_args()
    r = {'make': cmd_make, 'check': cmd_check, 'pages': cmd_pages, 'syms': cmd_syms}[a.cmd](a)
    sys.exit(r or 0)


if __name__ == '__main__':
    main()
