#!/usr/bin/env python3
"""sanity.py: scan FULL draw logs (MSC_VGX_DRAWLOG_FULL=1) for values no picture should contain: non-finite or huge floats in the
position / normal / texcoord slots of the first vertices, non-finite uniforms, draws with zero vertices or indices out of range.
  sanity.py DIR [--limit N]"""
import os, re, sys, collections
d = sys.argv[1]
lim = float(sys.argv[2]) if len(sys.argv) > 2 else 1e6
bad = collections.Counter(); tot = 0; ex = {}
for f in sorted(os.listdir(d)):
    cur = lay = None
    for line in open(os.path.join(d, f)):
        if line.startswith('D '):
            cur = line.split(' ')[1]; tot += 1
            lay = int(re.search(r'layout=(\w+)', line).group(1), 16)
            m = re.search(r'\bn=(\d+) idx=(\d+)', line)
            if m and (int(m.group(1)) == 0 or int(m.group(2)) == 0): bad['empty draw'] += 1
        elif line.startswith('  v'):
            toks = line.split(':')[1].split()
            # layout: pos (4 words incl. pad), [nrm 3], [col0 1], [col1 1], [tex 2 each], [tmi 2]
            slots = []
            for i, t in enumerate(toks):
                kind = 'pos' if i < 3 else None
                if i == 3: kind = None
                slots.append(kind)
            o = 4
            if lay & 1:
                for k in range(3): slots[o + k] = 'nrm'
                o += 3
            if lay & 2: slots[o] = 'col'; o += 1
            if lay & 4: slots[o] = 'col'; o += 1
            for tn in range(8):
                if lay & (8 << tn):
                    slots[o] = slots[o + 1] = 'tex'; o += 2
            for i, t in enumerate(toks):
                if slots[i] in ('pos', 'nrm', 'tex'):
                    if re.fullmatch(r'[0-9a-f]{8}', t) and not re.fullmatch(r'0+', t):
                        bad['non-finite %s' % slots[i]] += 1; ex.setdefault('nf ' + slots[i], (f, cur, line.strip()[:140]))
                    else:
                        try:
                            if abs(float(t)) > lim: bad['huge %s' % slots[i]] += 1; ex.setdefault('huge ' + slots[i], (f, cur, line.strip()[:140]))
                        except ValueError: pass
        elif line.startswith('  ub'):
            for t in line.split(':')[1].split():
                if re.fullmatch(r'(-?inf|-?nan)', t): bad['non-finite uniform'] += 1; ex.setdefault('uni', (f, cur, line[:100]))
print('draws', tot, dict(bad))
for k, v in ex.items(): print(' ', k, v)
