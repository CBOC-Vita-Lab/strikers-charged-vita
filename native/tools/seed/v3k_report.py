#!/usr/bin/env python3
"""W5-SEED: reads the game's log of a Vita3K run (build/np/W5SEED/v3k-runs/<tag>/log.txt) and reports what the shader work looked like.

  v3k_report.py <log.txt> [--after-frame N]

Prints: the warm-up list line, the seed copy line, the first-frame shader work, every later present that came >= 100 ms late with new shaders/programs
('present N came X ms after the previous one: +a vertex shaders ...' = a first use in the middle of play), the last 'shader work so far' line, the number of
'[vgx] frame N: ... shader compiles' lines after the start, the script's STATE lines.
"""
import re, sys
log = open(sys.argv[1], errors='replace').read().split('\n')
after = int(sys.argv[sys.argv.index('--after-frame') + 1]) if '--after-frame' in sys.argv else 2
pat_late = re.compile(r'present (\d+) came (\d+) ms after the previous one: \+(\d+) vertex shaders, \+(\d+) fragment shaders, \+(\d+) programs')
pat_frame = re.compile(r'\[vgx\] frame (\d+): (\d+) shader compiles \((\d+) answered by the simulated cache\), (\d+) program links, (\d+) ms of shader work')
late, work = [], []
for ln in log:
    if 'shader warm-up list' in ln or 'warm-up list from the package' in ln or 'compiled shaders from the package copied' in ln or 'no compiled shaders in the package' in ln:
        print(ln.strip()[:300])
    m = pat_late.search(ln)
    if m:
        late.append((int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5))))
    m = pat_frame.search(ln)
    if m:
        work.append((int(m.group(1)), int(m.group(2)), int(m.group(4)), int(m.group(5))))
    if 'shader work so far' in ln:
        last = ln.strip()
    if '[script]' in ln and 'STATE' in ln:
        print(ln.strip()[:160])
print('--- first frame(s) of shader work:')
for w in work[:3]:
    print('  frame %d: %d shader compiles, %d program links, %d ms' % w)
print('--- shader work in frames after the start (frame > %d): %d lines' % (after, sum(1 for w in work if w[0] > after)))
for w in [w for w in work if w[0] > after][:20]:
    print('  frame %d: %d shader compiles, %d program links, %d ms' % w)
print('--- presents that came late with new shaders (first use in the middle of play):', len([l for l in late if l[0] > after]))
for l in [l for l in late if l[0] > after][:20]:
    print('  present %d: %d ms late, +%d vs +%d fs +%d programs' % l)
try:
    print('--- ' + last)
except NameError:
    pass
