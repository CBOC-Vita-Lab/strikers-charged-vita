#!/usr/bin/env python3
"""Function-by-function difference of two ARM profiles (same script, same window).
  prof_cmp.py <tagA> <tagB> [g|o] [top=45] [frames=313]
g = game thread (default), o = render + audio threads. Uses tools/prof_split.py's symbolisation
(run with PROF_ALL=1). Prints the category totals of both and the functions whose cost changed most."""
import os, re, subprocess, sys
a, b = sys.argv[1], sys.argv[2]
th = sys.argv[3] if len(sys.argv) > 3 else 'g'
top = int(sys.argv[4]) if len(sys.argv) > 4 else 45
frames = sys.argv[5] if len(sys.argv) > 5 else '313'
here = os.path.dirname(os.path.abspath(__file__))
def load(tag):
    out = subprocess.run([sys.executable, here + '/prof_split.py', tag, frames, '400'], capture_output=True, text=True, env=dict(os.environ, PROF_ALL='1')).stdout
    part = out.split('== OTHER THREADS')
    txt = part[0] if th == 'g' else ('== OTHER THREADS' + part[1] if len(part) > 1 else '')
    cats, fns, head = {}, {}, ''
    for l in txt.splitlines():
        if l.startswith('=='): head = l; continue
        m = re.match(r'\s+([\d.]+) M\s+[\d.]+%\s+(.*)', l)
        if m: cats[m.group(2).strip()] = float(m.group(1)); continue
        m = re.match(r'\s+([\d.]+) M  (.*)', l)
        if m: fns[m.group(2).strip()[:110]] = fns.get(m.group(2).strip()[:110], 0) + float(m.group(1))
    return head, cats, fns
ha, ca, fa = load(a); hb, cb, fb = load(b)
print(a + ': ' + ha); print(b + ': ' + hb)
for k in sorted(set(ca) | set(cb), key=lambda k: -max(ca.get(k, 0), cb.get(k, 0))):
    print('  %-34s %7.2f %7.2f %+7.2f' % (k[:34], ca.get(k, 0), cb.get(k, 0), cb.get(k, 0) - ca.get(k, 0)))
print('functions by |change| (M instructions per frame):')
keys = sorted(set(fa) | set(fb), key=lambda k: -abs(fb.get(k, 0) - fa.get(k, 0)))
for k in keys[:top]:
    print('  %7.3f %7.3f %+7.3f  %s' % (fa.get(k, 0), fb.get(k, 0), fb.get(k, 0) - fa.get(k, 0), k))
