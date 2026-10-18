#!/usr/bin/env python3
"""windowcmp.py: drawdiff for windows of a nav-script run: A frames a0-a1 against the best B frame within +-S of (frame + OFFSET).

  windowcmp.py A_DIR B_DIR a0-a1:OFFSET [a0-a1:OFFSET ...] [--search S] [--ftol F] [--btol B] [--detail N]

(B_DIR may be a comma-separated list of directories: the first that has the frame.) Prints, per window, the best pair per A frame with
the equal / noise / DIFFERENT counts; --detail N prints the details of the N worst pairs.
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drawdiff

def main():
    av = sys.argv[1:]
    opt = {'search': 8, 'detail': 0}
    pos = []
    i = 0
    while i < len(av):
        if av[i] == '--search': opt['search'] = int(av[i + 1]); i += 2
        elif av[i] == '--detail': opt['detail'] = int(av[i + 1]); i += 2
        elif av[i] == '--ftol': drawdiff.OPT['ftol'] = float(av[i + 1]); i += 2
        elif av[i] == '--btol': drawdiff.OPT['btol'] = int(av[i + 1]); i += 2
        else: pos.append(av[i]); i += 1
    da = pos[0]; dbs = pos[1].split(',')
    def find(n):
        for d in dbs:
            p = os.path.join(d, 'f%06d.txt' % n)
            if os.path.exists(p): return p
    worst = []
    for w in pos[2:]:
        rng, off = w.split(':')
        a0, a1 = map(int, rng.split('-')); off = int(off)
        tot = {'equal': 0, 'noise': 0, 'diff': 0}
        nfr = 0; nbad = 0; offs = []
        for n in range(a0, a1 + 1):
            pa = os.path.join(da, 'f%06d.txt' % n)
            if not os.path.exists(pa): continue
            best = None
            for d in sorted(range(-opt['search'], opt['search'] + 1), key=abs):
                pb = find(n + off + d)
                if not pb: continue
                r = drawdiff.compare(pa, pb)
                key = (r[2]['diff'], r[2]['noise'])
                if best is None or key < best[0]: best = (key, n + off + d, r, pb)
            if not best: continue
            nfr += 1
            for k in tot: tot[k] += best[2][2][k]
            offs.append(best[1] - n)
            if best[2][2]['diff']: nbad += 1; worst.append((best[2][2]['diff'], n, best[1], pa, best[3], best[2]))
        print('window %d-%d (offset %d): %d frames compared, draws+copies: %d equal, %d noise, %d DIFFERENT (in %d frames); best offsets %s' % (a0, a1, off, nfr, tot['equal'], tot['noise'], tot['diff'], nbad, sorted(set(offs))))
    worst.sort(reverse=True)
    for d, n, m, pa, pb, r in worst[:opt['detail']]:
        print('--- frame %d vs %d: %d DIFFERENT' % (n, m, d))
        print('\n'.join(r[3][:30]))

if __name__ == '__main__':
    main()
