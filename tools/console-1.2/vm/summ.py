#!/usr/bin/env python3
# summ.py [-v] <log.txt>... : one line of the acceptance figures per console log (open-play windows only where it says play).
import re, sys, statistics as st
def summ(path, verbose=False):
    t = open(path, 'rb').read().decode('utf-8', 'replace').replace('\r', '')
    L = t.split('\n'); play = False; fps=[]; game=[]; gpu=[]; p50=[]; p95=[]; ph=[]; allfps=[]
    hc = {}; gaps=[0,0,0]; kick=[]; seen_kick=False; holds=[]; held=[0,0,0,0,0]; jud=[]
    live=True; dirty=False; dirtyp=False; dirtyl=False
    for l in L:
        if l.startswith('[hold] scene '):
            live = l.rstrip().endswith('-> play') or '-> play:' in l or '-> play ' in l
            dirty = dirtyp = dirtyl = True
        elif not live: dirty = dirtyp = dirtyl = True
        m = re.search(r'@(begin|end) ([\w-]*play)', l)
        if m: play = (m.group(1) == 'begin'); continue
        if 'STATE kick-off' in l: seen_kick = True
        if l.startswith('[lane] '):
            m = re.search(r'pics [\d.]+s ([\d.]+)/s game ([\d.]+).* gpu ([\d.]+)', l)
            if m:
                f, g, u = map(float, m.groups()); allfps.append(f)
                if seen_kick and len(kick) < 2: kick.append(f)
                if play and not dirty: fps.append(f); game.append(g); gpu.append(u)
                dirty = not live
        elif l.startswith('[lat] ') and play and dirtyl: dirtyl = not live
        elif l.startswith('[lat] ') and play:
            m = re.search(r'pad->shown p50 ([\d.]+) p95 ([\d.]+)', l)
            if m: p50.append(float(m.group(1))); p95.append(float(m.group(2)))
            m = re.search(r'presented->handed p50 ([\d.]+)', l)
            if m: ph.append(float(m.group(1)))
        elif l.startswith('[present] ') and play and dirtyp: dirtyp = not live
        elif l.startswith('[present] ') and play:
            m = re.search(r'held 0/1/2/3/4\+ vblanks (\d+)/(\d+)/(\d+)/(\d+)/(\d+)', l)
            if m:
                for i in range(5): held[i] += int(m.group(i+1))
            m = re.search(r'judder ([\d.]+)', l)
            if m: jud.append(float(m.group(1)))
        elif l.startswith('[hitch] '):
            w = l.split()
            if len(w) > 2 and w[1].startswith('gap='):
                hc[w[2]] = hc.get(w[2], 0) + 1
                try: g = float(w[1][4:])
                except ValueError: g = 0
                gaps[0 if g < 50 else 1 if g < 100 else 2] += 1
        elif l.startswith('[hold] ') and ('step' in l or 'match starts' in l): holds.append(l[:110])
    lp = [int(x) for x in re.findall(r'^\[vgx-vita\] present \d+ came (\d+) ms', t, re.M)]
    big = [x for x in lp if 150 <= x < 1000]
    seg = [(int(a), int(b)) for a, b in re.findall(r'^\[frame-timing\] reason=identity .*? n=(\d+) .*?p50_us=(\d+)', t, re.M)]
    slow = [(n, p) for n, p in seg if n >= 30 and p > 45000]
    sp = [float(x) for x in re.findall(r'^\[speed\][^\n]*?([\d.]+) ?%', t, re.M)]
    done = bool(re.search(r'^\[script\] f=\d+ exit|\[run\] end', t, re.M))
    mean = lambda a: (sum(a)/len(a)) if a else float('nan')
    hs = sum(hc.values())
    print(f"{(path.split('/')[-2:-1] or ['.'])[0]:14s} {'done' if done else 'CUT '} play {len(fps):2d}w fps {mean(fps):5.1f} (min {min(fps) if fps else 0:4.1f}) game {mean(game):5.1f} gpu {mean(gpu):5.1f} | kick {'/'.join('%.0f'%k for k in kick) or '-'} | press p50 {mean(p50):6.1f} p95 {mean(p95):6.1f} wait {mean(ph):5.1f} | hitch {hs:3d} up {hc.get('upstream',0):3d} disp {hc.get('display',0):3d} tex {hc.get('texture',0):3d} sh {hc.get('shader',0):2d} gaps {gaps[0]}/{gaps[1]}/{gaps[2]} | held1/2/3+ {held[1]}/{held[2]}/{held[3]+held[4]} ({100.0*(held[3]+held[4])/max(1,sum(held)):.1f}% late) judder {mean(jud):.2f} | cuts {len(big)} pauses (worst {max(big) if big else 0} ms) slow-scenes {sum(n for n,p in slow)} pics (worst median {max([p for n,p in slow])/1000 if slow else 0:.0f} ms) | speed {('%.1f' % min(sp)) if sp else '-'}..{('%.1f' % max(sp)) if sp else '-'}")
    if verbose:
        for h in holds[:3] + holds[-3:]: print('   ', h)
        for tag in ('[hunt] window', '[gsplit', '[env]', '[clock] OVERLOAD', '[onlinesave]', '[fpscr]', '[tlsverify', '[quiesce'):
            x = [l for l in L if l.startswith(tag)]
            if x: print('   ', len(x), 'x', x[-1][:230])
if __name__ == '__main__':
    a = sys.argv[1:]; v = '-v' in a
    for p in [x for x in a if x != '-v']:
        try: summ(p, v)
        except Exception as e: print(p, 'ERROR', e)
