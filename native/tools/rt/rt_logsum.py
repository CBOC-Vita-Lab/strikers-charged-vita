#!/usr/bin/env python3
"""W5 RT: summary of a game log (console, Vita3K or host32): the [rt] lines (scenes, depth traffic, CPU of scene changes, GPU time), the [lat] lines
(input delay and its stages), the [frame-timing] windows (frame interval p50/p95, lat_* fields) and the display gate's held-vblank line.

  python3 -I tools/rt/rt_logsum.py <log.txt> [--from N]    (--from: skip [rt]/[lat] lines before the N-th presented frame, e.g. the menus)
"""
import re, sys


def nums(s):
    return [float(x) for x in re.findall(r'-?\d+(?:\.\d+)?', s)]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    lines = open(path, errors='replace').read().splitlines()
    rt = [l for l in lines if l.startswith('[rt] ') and 'frames: scenes' in l]
    lat = [l for l in lines if l.startswith('[lat] ')]
    ft = [l for l in lines if l.startswith('[frame-timing] ')]
    held = [l for l in lines if 'display: interval' in l and 'pictures were shown' in l]
    print('%s: %d [rt] lines, %d [lat] lines, %d [frame-timing] windows' % (path, len(rt), len(lat), len(ft)))
    for l in rt[-6:]:
        m = re.search(r'scenes ([\d.]+)/frame.*?not loaded ([\d.]+).*?not stored ([\d.]+).*?loaded ([\d.]+) stored ([\d.]+), not loaded ([\d.]+), not stored ([\d.]+)', l)
        c = re.search(r'CPU scene change ([\d.]+) ms/frame', l)
        d = re.search(r'draw that begins a scene ([\d.]+) us \(x([\d.]+)/frame\), other draws ([\d.]+) us', l)
        g = re.search(r'busy p50 ([\d.]+) p95 ([\d.]+) ms, first submit to last done p50 ([\d.]+) p95 ([\d.]+) ms, last submit to done p50 ([\d.]+) p95 ([\d.]+)', l)
        out = []
        if m:
            out.append('scenes %s, noload %s nostore %s, depth MB/frame moved %.2f, not moved %.2f' % (m.group(1), m.group(2), m.group(3),
                       float(m.group(4)) + float(m.group(5)), float(m.group(6)) + float(m.group(7))))
        if c:
            out.append('CPU switch %s ms' % c.group(1))
        if d:
            out.append('first draw %s us x%s, other %s us' % d.groups())
        if g:
            out.append('GPU busy p50/p95 %s/%s ms span %s/%s tail %s/%s' % g.groups())
        print('  [rt] ' + '; '.join(out))
    for l in lat[-6:]:
        print('  ' + l[:400])
    for l in ft:
        m = re.search(r'n=(\d+) .*?p50_us=(\d+) p95_us=(\d+).*?task=(\d+).*?lat_n=(\d+) lat_p50_us=(\d+) lat_p95_us=(\d+)', l)
        if m and int(m.group(1)) >= 100:
            n, p50, p95, task, ln, lp50, lp95 = m.groups()
            print('  window n=%s task=%s frame p50 %.1f p95 %.1f ms | input delay n=%s p50 %.1f p95 %.1f ms' % (n, task, int(p50) / 1000, int(p95) / 1000, ln,
                  int(lp50) / 1000, int(lp95) / 1000))
    for l in held[-3:]:
        print('  ' + l[l.index('display:'):][:200])
    return 0


if __name__ == '__main__':
    sys.exit(main())
