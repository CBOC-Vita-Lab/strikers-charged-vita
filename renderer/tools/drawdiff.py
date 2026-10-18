#!/usr/bin/env python3
"""drawdiff.py: compare two MSC_VGX_DRAWLOG frame logs (np branch; vgx_draw.cpp, namespace drawlog).

  drawdiff.py A/fNNNNNN.txt B/fMMMMMM.txt [--max N]            two files
  drawdiff.py DIR_A DIR_B [--map OFFSET] [--max N] [--quiet]   every frame present in both (B frame = A frame + OFFSET)
  options: --ftol 1e-5 (relative float tolerance)  --btol 2 (per-byte tolerance of packed vertex words)  --hard (only list hard differences)
           --search N  (directory mode) compare each frame of A with the frames n-N..n+N of B (after --map) and take the best: scenes that are
           time based (movies, fades) sit at slightly different frame numbers in the two builds. Uploads ('U') are not compared per frame
           (they happen in the frame that first needs a texture); the texture content hashes in the draws carry the information.

Lines: 'U' texture uploads, 'D' draws (key=value fields), 'CT' / 'CD' copies. Draws are compared by position.
A draw is
  equal      all fields equal;
  noise      only the content hashes of the uniforms / vertices differ (geo, ubN) and the full values (logs made with
             MSC_VGX_DRAWLOG_FULL=1) agree within the tolerances: floating-point evaluation order (PPC fused multiply-add
             against the host's), colour bytes off by one. Reported as a count, not as a problem;
  DIFFERENT  anything else: state, shader, texture, counts, or values beyond the tolerance (listed with the first values).
Without FULL logs the hash differences cannot be classified and count as DIFFERENT (marked 'hash only').
"""
import os, re, sys

OPT = {'ftol': 1e-5, 'btol': 2, 'max': 12, 'map': 0, 'search': 0, 'summary': False}
SOFT = re.compile(r'^(geo|ub\d)$')


def parse(path):
    recs = []
    with open(path) as f:
        cur = None
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('  '):
                if cur is not None:
                    cur['extra'][line.split(':')[0].strip()] = line.split(':', 1)[1].split()
                continue
            if line.startswith('frame '):
                continue
            kind = line.split(' ', 1)[0]
            fields = {}
            for p in line.split(' ')[1:]:
                if '=' in p:
                    k, v = p.split('=', 1)
                    fields[k] = v
            cur = {'kind': kind, 'f': fields, 'extra': {}, 'raw': line}
            recs.append(cur)
    return recs


def tok_diff(x, y):
    """None if equal within tolerance, else a short description."""
    if x == y:
        return None
    hx = re.fullmatch(r'[0-9a-f]{8}', x) is not None
    hy = re.fullmatch(r'[0-9a-f]{8}', y) is not None
    if hx and hy:
        bx, by = bytes.fromhex(x), bytes.fromhex(y)
        if all(abs(a - b) <= OPT['btol'] for a, b in zip(bx, by)):
            return None
        return '%s vs %s' % (x, y)
    try:
        a, b = float(x), float(y)
    except ValueError:
        return '%s vs %s' % (x, y)
    if abs(a - b) <= OPT['ftol'] * max(1.0, abs(a), abs(b)):
        return None
    return '%s vs %s' % (x, y)


def extra_diff(a, b):
    """(list of hard messages) for the FULL data of two draws."""
    msgs = []
    for name in sorted(set(a['extra']) | set(b['extra'])):
        va, vb = a['extra'].get(name), b['extra'].get(name)
        if va is None or vb is None:
            msgs.append('    %s present in one only' % name); continue
        if len(va) != len(vb):
            msgs.append('    %s: %d vs %d values' % (name, len(va), len(vb))); continue
        bad = [(i, d) for i, (x, y) in enumerate(zip(va, vb)) for d in [tok_diff(x, y)] if d]
        if bad:
            msgs.append('    %s: %d of %d values differ beyond tolerance; first: %s' % (name, len(bad), len(va), ', '.join('[%d] %s' % t for t in bad[:5])))
    return msgs


def classify(x, y, full):
    """'equal' | 'noise' | ('diff', [messages])"""
    if x['f'] == y['f']:
        return 'equal', []
    # tlN = authored mip levels of texture N: a back end / recompiled-build matter (the recompiled build's texture objects forget the game's LOD range
    # for copied objects), not a difference in what the game asked for; compared only when nothing else differs
    strip = lambda r: {k: v for k, v in r['f'].items() if not re.match(r'tl\d$', k)}
    if strip(x) == strip(y):
        return 'noise', []
    hard = [k for k in set(x['f']) | set(y['f']) if x['f'].get(k) != y['f'].get(k) and not SOFT.match(k) and not re.match(r'tl\d$', k)]
    if hard:
        return 'diff', ['  %s %s: %s' % (x['kind'], x['raw'].split(' ')[1] if x['kind'] == 'D' else '', ' '.join('%s: %s -> %s' % (k, x['f'].get(k), y['f'].get(k)) for k in sorted(hard)))]
    soft = sorted(k for k in set(x['f']) | set(y['f']) if x['f'].get(k) != y['f'].get(k))
    if not full or not x['extra'] or not y['extra']:
        return 'diff', ['  D %s: hash only (%s): no FULL log to classify' % (x['raw'].split(' ')[1], ' '.join(soft))]
    m = extra_diff(x, y)
    if m:
        return 'diff', ['  D %s: %s differ' % (x['raw'].split(' ')[1], ' '.join(soft))] + m
    return 'noise', []


def compare(pa, pb):
    A, B = parse(pa), parse(pb)
    full = any(r['extra'] for r in A) and any(r['extra'] for r in B)
    out, cnt = [], {'equal': 0, 'noise': 0, 'diff': 0}
    na = sum(1 for r in A if r['kind'] == 'D')
    nb = sum(1 for r in B if r['kind'] == 'D')
    for kind in ('D', 'CT', 'CD'):
        ra = [r for r in A if r['kind'] == kind]
        rb = [r for r in B if r['kind'] == kind]
        if len(ra) != len(rb):
            out.append('  %s: %d records vs %d' % (kind, len(ra), len(rb)))
            cnt['diff'] += 1
        # align by the fields that are not hashes of values (an extra or a missing draw must not shift the rest)
        import difflib
        sig = lambda r: tuple(sorted((k, v) for k, v in r['f'].items() if not SOFT.match(k) and not re.match(r'tl\d$', k)))
        sm = difflib.SequenceMatcher(None, [sig(r) for r in ra], [sig(r) for r in rb], autojunk=False)
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag == 'equal':
                pairs = list(zip(ra[i1:i2], rb[j1:j2]))
            else:
                pairs = list(zip(ra[i1:i2], rb[j1:j2]))
                for x in ra[i1 + len(pairs):i2]:
                    cnt['diff'] += 1
                    out.append('  %s %s only in A: %s' % (kind, x['raw'].split(' ')[1] if kind == 'D' else '', x['raw'][:300]))
                for y in rb[j1 + len(pairs):j2]:
                    cnt['diff'] += 1
                    out.append('  %s %s only in B: %s' % (kind, y['raw'].split(' ')[1] if kind == 'D' else '', y['raw'][:300]))
            for x, y in pairs:
                c, m = classify(x, y, full)
                cnt[c] += 1
                out.extend(m)
    return na, nb, cnt, out


def main():
    av = sys.argv[1:]
    args = []
    i = 0
    while i < len(av):
        a = av[i]
        if a in ('--quiet', '--hard', '--summary'):
            OPT[a[2:]] = True
        elif a.startswith('--'):
            OPT[a[2:]] = type(OPT.get(a[2:], 0))(av[i + 1]) if a[2:] in OPT else av[i + 1]
            i += 1
        else:
            args.append(a)
        i += 1
    a, b = args[0], args[1]
    if os.path.isfile(a):
        na, nb, cnt, out = compare(a, b)
        print('draws %d vs %d: %d equal, %d noise, %d DIFFERENT' % (na, nb, cnt['equal'], cnt['noise'], cnt['diff']))
        print('\n'.join(out[:OPT['max'] * 4]))
        return 1 if cnt['diff'] else 0
    fa = {int(m.group(1)): m.group(0) for m in (re.fullmatch(r'f(\d+)\.txt', f) for f in os.listdir(a)) if m}
    fb = {int(m.group(1)): m.group(0) for m in (re.fullmatch(r'f(\d+)\.txt', f) for f in os.listdir(b)) if m}
    bad = noisy = same = 0
    sigs = {}
    for n in sorted(fa):
        best = None
        for d in sorted(range(-OPT['search'], OPT['search'] + 1), key=abs):
            m = n + OPT['map'] + d
            if m not in fb: continue
            r = compare(os.path.join(a, fa[n]), os.path.join(b, fb[m]))
            key = (r[2]['diff'], r[2]['noise'])
            if best is None or key < best[0]: best = (key, m, r)
            if key == (0, 0): break
        if best is None: continue
        mm = best[1]
        na, nb, cnt, out = best[2]
        if cnt['diff']:
            bad += 1
        elif cnt['noise']:
            noisy += 1
        else:
            same += 1
        if OPT.get('summary'):
            for line in out:
                if line.startswith('    '): continue
                # signature: the kind of record and the names of the differing fields (values removed)
                m = re.match(r'  (\w+) (\d+)(?: only in (A|B))?(.*)', line)
                if not m: sig = line[:60]
                else:
                    names = sorted(set(re.findall(r'\b(vs|fs|geo|layout|n|idx|tm|t\d|rs|ub\d)\b', m.group(4).split(':')[0] if m.group(3) is None else '')))
                    sig = '%s %s %s' % (m.group(1), ('only in ' + m.group(3)) if m.group(3) else 'differs', ','.join(names))
                e = sigs.setdefault(sig, [0, n])
                e[0] += 1
        if OPT.get('summary'):
            if cnt['diff']: bad += 1
            elif cnt['noise']: noisy += 1
            else: same += 1
            continue
        if cnt['diff'] or (cnt['noise'] and not OPT.get('hard')):
            print('frame %d vs %d: draws %d vs %d: %d equal, %d noise, %d DIFFERENT' % (n, mm, na, nb, cnt['equal'], cnt['noise'], cnt['diff']))
            if cnt['diff'] and not OPT.get('quiet'): print('\n'.join(out[:OPT['max'] * 4]))
    if OPT.get('summary'):
        for sig, (c, first) in sorted(sigs.items(), key=lambda kv: -kv[1][0])[:40]:
            print('%6d x  %s   (first in frame %d)' % (c, sig, first))
    print('frames: %d identical, %d with float/byte noise only, %d with real differences' % (same, noisy, bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
