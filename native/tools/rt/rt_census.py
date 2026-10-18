#!/usr/bin/env python3
"""W5 RT: the scene census of a run (MSC_VGX_GLSTATS csv of the vgx GL back end, counters rt* of src/vgx_backend_gl.cpp) by segment.

  python3 -I tools/rt/rt_census.py run/glstats.csv [name:first-last ...]

Without segments: menus (frames with 20..250 draws) and open play (more than 250 draws), the split GLB used. Per segment, means per frame:
scenes (EFB / pass target / copy destination / progress display), scenes whose first operation is a clear (of the whole depth buffer),
scenes with a depth-tested draw, scenes with no draw or copy quad, returns to the target of the scene before the previous one, and with
MSC_VGX_RT=1 the scenes given NOLOAD / NOSTORE and the depth bytes a scene loads + stores on vitaGL (STORE_DEPTH_STENCIL) against those
the hints leave unmoved. The display's own scene (the present) is the host's and not in these counters: +1 scene a frame, 960x544 DF32M_S8
= 2.6 MB loaded + 2.6 MB stored without the hint.
"""
import csv, sys

COLS = ['scenes', 'efbScenes', 'rtPassScenes', 'rtCopyScenes', 'rtDispScenes', 'rtClearFirst', 'rtFullDepthFirst', 'rtDepthScenes',
        'rtEmptyScenes', 'rtReturns', 'rtNoLoad', 'rtNoStore', 'rtDsKib', 'rtDsSkipKib', 'rtDeferredClears', 'rtSimClears', 'rtHeldProgress',
        'draws', 'copyQuads', 'clears', 'fboBinds', 'fboAttach']


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    rows = list(csv.DictReader(open(sys.argv[1])))
    segs = []
    for a in sys.argv[2:]:
        name, _, rng = a.partition(':')
        lo, _, hi = rng.partition('-')
        segs.append((name, lambda r, lo=int(lo), hi=int(hi): lo <= int(r['frame']) <= hi))
    if not segs:
        segs = [('menus', lambda r: 20 <= int(r['draws']) <= 250), ('open play', lambda r: int(r['draws']) > 250)]
    cols = [c for c in COLS if rows and c in rows[0]]
    print('segment'.ljust(14) + 'frames'.rjust(7) + ''.join(c.replace('rt', '').rjust(13) for c in cols))
    for name, pred in segs:
        sel = [r for r in rows if pred(r)]
        if not sel:
            continue
        line = name.ljust(14) + str(len(sel)).rjust(7)
        for c in cols:
            line += ('%.2f' % (sum(float(r[c]) for r in sel) / len(sel))).rjust(13)
        print(line)
        ds = sum(float(r.get('rtDsKib', 0)) for r in sel) / len(sel) / 1024.0
        sk = sum(float(r.get('rtDsSkipKib', 0)) for r in sel) / len(sel) / 1024.0
        print('   depth traffic of the back end\'s scenes on vitaGL: %.2f MB/frame loaded+stored, %.2f MB/frame left unmoved by the hints (%.0f %%)'
              % (ds, sk, 100.0 * sk / ds if ds else 0.0))
    return 0


if __name__ == '__main__':
    sys.exit(main())
