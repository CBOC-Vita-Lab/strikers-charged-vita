#!/usr/bin/env python3
"""W5-ITEM: reads the runs of tools/item/scripts (ITEM.run.sh: build/np/W5ITEM/runs/<scenario>_<cfg>/run.log, det.trace) and prints
   - per scenario and configuration: exit, frames, simulation steps, per phase (open / goal / cycle): given, used, objects, hits on fielders /
     keepers, effects on the other team and on the own team, the cycle (Z put the mushroom in front);
   - the determinism checks: B against A and C against D (the rendering switches alone, at camera level 3 and 5), step by step (det.trace);
   - frames for pictures (--frames): per phase the moment before the give, the use, the first hit / effect and later.
   report.py [--runs DIR] [--md] [--frames OUT.json] [--cfg A] [scenario regex]"""
import argparse, json, os, re, sys

FX_OTHER_IGNORE = set()


def parse(path):
    r = {'exit': '?', 'frames': 0, 'sim': 0, 'phases': {}, 'order': [], 'sum': [], 'errors': []}
    cur = None
    lastf = 0
    if not os.path.exists(path):
        r['exit'] = 'missing'
        return r
    with open(path, errors='replace') as f:
        for line in f:
            m = re.match(r'\[script\] f=(\d+) ITEM (\S+) phase (\w+)', line)
            if m:
                cur = m.group(3)
                r['order'].append(cur)
                r['phases'][cur] = ph = {'f': int(m.group(1)), 'given': 0, 'notgiven': 0, 'used': 0, 'objs': [], 'hitsF': 0, 'hitsK': 0,
                                         'fxOther': {}, 'fxOwn': {}, 'chomp': 0, 'cycled': False, 'zswap': False, 'fuse': None, 'fobj': None,
                                         'fhit': None, 'ffx': None, 'fend': None, 'timeouts': 0}
                continue
            m = re.match(r'\[script\] f=(\d+)', line)
            if m:
                lastf = int(m.group(1))
            if cur:
                ph = r['phases'][cur]
                if re.match(r'\[script\] f=\d+ ITEM \S+ (use|cycle press)', line):
                    if ph['fuse'] is None or 'use' in line:
                        ph['fuse'] = int(re.match(r'\[script\] f=(\d+)', line).group(1))
                if 'cycled: mushroom in front' in line:
                    ph['cycled'] = True
                if 'timeout' in line and line.startswith('[script]'):
                    ph['timeouts'] += 1
            m = re.match(r'\[item\] sim=(\d+) give side=(\d) (\S+) x(\d+) (\w+): (.*)', line)
            if m and cur:
                ph = r['phases'][cur]
                if 'NOT given' in m.group(6):
                    ph['notgiven'] += 1
                else:
                    ph['given'] += 1
            m = re.match(r'\[item\] f=(\d+) sim=(\d+) slots side=0: (\S+) x(\d+) \| (\S+) x(\d+)', line)
            if m and cur:
                ph = r['phases'][cur]
                if m.group(3) == 'mushroom' and m.group(5) != 'none':
                    ph['zswap'] = True
            m = re.match(r'\[item\] f=(\d+) sim=(\d+) object (\d+) new: (\S+) (\S+)(.*) thrower side(-?\d)#', line)
            if m and cur:
                ph = r['phases'][cur]
                if m.group(7) != '0':   # an object the other team threw (the AI's own items) is counted apart
                    ph.setdefault('theirs', []).append(m.group(4))
                    continue
                ph['objs'].append('%s %s%s' % (m.group(4), m.group(5), ' exploder' if 'exploder' in m.group(6) else ''))
                if ph['fobj'] is None:
                    ph['fobj'] = int(m.group(1))
            m = re.match(r'\[item\] sim=(\d+) hit (\S+) \((\w+)\) thrown by side(-?\d)#(-?\d) -> (KEEPER|fielder) side(-?\d)#', line)
            if m and cur:
                ph = r['phases'][cur]
                if m.group(6) == 'KEEPER':
                    ph['hitsK'] += 1
                else:
                    ph['hitsF'] += 1
                ph['_pendhit'] = True
            m = re.match(r'\[item\] f=(\d+) sim=(\d+) (?:chain chomp OUT)', line)
            if m and cur:
                r['phases'][cur]['chomp'] += 1
            m = re.match(r'\[item\] f=(\d+) sim=(\d+) fx side(\d)#(\d): (.*)', line)
            if m and cur:
                ph = r['phases'][cur]
                fr = int(m.group(1))
                for tok in m.group(5).split():
                    if tok.startswith('+'):
                        d = ph['fxOther'] if m.group(3) == '1' else ph['fxOwn']
                        d[tok[1:]] = d.get(tok[1:], 0) + 1
                        if m.group(3) == '1' and ph['ffx'] is None and (ph['fuse'] is not None and fr >= ph['fuse']):
                            ph['ffx'] = fr
                if ph.get('_pendhit') and ph['fhit'] is None:
                    ph['fhit'] = fr
            m = re.match(r'\[item\] f=(\d+) sim=(\d+) slots side=0: none x0 \| none x0', line)
            if m and cur:
                ph = r['phases'][cur]
                if ph['fuse'] is not None:
                    ph['used'] += 1
            if line.startswith('[itemsum]'):
                r['sum'].append(line.strip())
            if 'script exit' in line:
                r['exit'] = 'exit'
            elif '[script] TIMEOUT' in line:
                r['exit'] = 'TIMEOUT'
            elif 'reached MSC_MAX_FRAMES' in line:
                r['exit'] = 'maxframes'
            m = re.match(r'\[watch\] f=(\d+) .* sim=(\d+)', line)
            if m:
                r['frames'] = int(m.group(1))
                r['sim'] = int(m.group(2))
            if re.search(r'Segmentation|SIGSEGV|Aborted|\[crash\]|assert', line, re.I) and not line.startswith('[script]'):
                r['errors'].append(line.strip()[:120])
    r['lastf'] = lastf
    # cut-scenes and replays (task states 16 / 8) with their first frame: the replay pictures of the rp_ scenarios
    r['nis'] = []
    with open(path, errors='replace') as f:
        for line in f:
            m = re.match(r'\[nav\] f=(\d+) task state (\d+) -> (\d+)', line)
            if m and m.group(3) in ('8', '16') and r['order']:
                r['nis'].append((int(m.group(1)), int(m.group(3))))
    return r


def det_cmp(a, b):
    def load(p):
        d = {}
        if not os.path.exists(p):
            return d
        with open(p, errors='replace') as f:
            for line in f:
                w = line.split()
                if len(w) == 17:
                    d[int(w[0])] = w[1:]
        return d
    A, B = load(a), load(b)
    common = sorted(set(A) & set(B))
    diff = [s for s in common if A[s] != B[s]]
    return len(A), len(B), len(common), len(diff), (diff[0] if diff else None)


def phase_text(ph):
    if ph is None:
        return '-'
    out = []
    if ph['notgiven']:
        out.append('NOT GIVEN')
    if ph['objs']:
        kinds = sorted(set(ph['objs']))
        out.append('%d obj (%s)' % (len(ph['objs']), ', '.join(kinds)))
    if ph.get('theirs'):
        out.append('(AI threw %s)' % ', '.join(sorted(set(ph['theirs']))))
    if ph['chomp']:
        out.append('chomp out')
    if ph['hitsF'] or ph['hitsK']:
        out.append('hits %d fielder%s' % (ph['hitsF'], ', %d keeper' % ph['hitsK'] if ph['hitsK'] else ''))
    if ph['fxOther']:
        out.append('them: ' + ' '.join('%s%s' % (k, '' if v == 1 else 'x%d' % v) for k, v in sorted(ph['fxOther'].items())))
    own = {k: v for k, v in ph['fxOwn'].items() if k in ('giant', 'super', 'star', 'mushroom', 'shrunk')}
    if own:
        out.append('us: ' + ' '.join(sorted(own)))
    if ph['timeouts']:
        out.append('%d wait timeouts' % ph['timeouts'])
    return '; '.join(out) or 'nothing seen'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--runs', default='/home/claude/msc/build/np/W5ITEM/runs')
    ap.add_argument('--md', action='store_true')
    ap.add_argument('--frames', default='')
    ap.add_argument('--cfg', default='A')
    ap.add_argument('pattern', nargs='?', default='.')
    a = ap.parse_args()
    names = sorted(set(re.sub(r'_[ABCD]$', '', d) for d in os.listdir(a.runs) if re.search(r'_[ABCD]$', d)))
    names = [n for n in names if re.search(a.pattern, n)]
    rows = []
    frames = {}
    for n in names:
        R = {c: parse(os.path.join(a.runs, '%s_%s' % (n, c), 'run.log')) for c in 'ABCD'}
        base = R[a.cfg]
        dets = {}
        for x, y in (('A', 'B'), ('D', 'C')):
            pa, pb = os.path.join(a.runs, '%s_%s' % (n, x), 'det.trace'), os.path.join(a.runs, '%s_%s' % (n, y), 'det.trace')
            dets[x + y] = det_cmp(pa, pb)
        ex = ' '.join('%s:%s' % (c, R[c]['exit']) for c in 'ABCD')
        errs = sum(len(R[c]['errors']) for c in 'ABCD')
        det = ' '.join('%s=%s %d/%d%s' % (k[1], k[0], v[2] - v[3], v[2], '' if v[3] == 0 else ' first diff step %s' % v[4]) for k, v in dets.items())
        P = base['phases']
        rows.append((n, phase_text(P.get('open')), phase_text(P.get('goal')),
                     ('Z ok' if (P.get('cycle') or {}).get('zswap') else 'Z NOT SEEN') + '; ' + phase_text(P.get('cycle')), ex, det, errs))
        fl = {}
        for c in 'ABCD':
            L = []
            for pn, ph in R[c]['phases'].items():
                cand = [ph['f'] + 2]
                if ph['fuse']:
                    cand.append(ph['fuse'] + 4)
                    t = ph['ffx'] or ph['fhit'] or ph['fobj']
                    if t:
                        cand += [t + 3, t + 30]
                    else:
                        cand += [ph['fuse'] + 20, ph['fuse'] + 60]
                L += cand
            # cut-scenes (task 16: walk-out, celebrations) and replays (task 8): CPU-skinned characters (GPU-skinned with MSC_SKIN_GPU=1)
            for f0, st in R[c]['nis'][:(8 if n.startswith('rp_') else 4)]:
                L += [f0 + 30, f0 + 90] if st == 8 else [f0 + 20]
            fl[c] = sorted(set(L))
        frames[n] = fl
    if a.md:
        print('| scenario | open play (at an opponent) | near the goal (at the keeper) | cycle (Z) and use | exit A B C D | sim B=A, C=D (steps equal / common) | errors |')
        print('|---|---|---|---|---|---|---|')
        for r in rows:
            print('| %s | %s | %s | %s | %s | %s | %d |' % r)
    else:
        for r in rows:
            print('%-14s\n  open:  %s\n  goal:  %s\n  cycle: %s\n  exit:  %s\n  det:   %s   errors %d' % r)
    if a.frames:
        with open(a.frames, 'w') as f:
            json.dump(frames, f, indent=0)


if __name__ == '__main__':
    main()
