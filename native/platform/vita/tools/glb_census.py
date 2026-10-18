#!/usr/bin/env python3
"""GLB (wave 5): per-frame means of the GL call census (MSC_VGX_GLSTATS csv) over the open-play frames of one or more runs, and the estimated cost of
the vitaGL back end per frame from the console unit costs. [E] = estimate.

  python3 -I platform/vita/tools/glb_census.py [--min-draws N] [--log run/log.txt ...] run-a/glstats.csv|run-a/log.txt [run-b/glstats.csv ...]
  python3 -I platform/vita/tools/glb_census.py --scenarios run/glstats.csv [--log run/log.txt]

Open play = frames with more than --min-draws draws (default 250; the menus and the title screen have 20-150). The columns are the counters of
src/vgx_backend_gl.cpp (VGX_GL_COUNTERS). The cost model uses what the console measured on 5 Oct (docs/wave4-VRS.md 4.1) minus the profiler's own cost
(docs/wave5-GLB.md 1): a draw command 19.8 us net, the stream upload 3.9 ms per MB (uncached), the ring 3.9 ms per MB (the same uncached write), the rest of
the net 11.8 ms (copies, pass begins, scene changes, the loop between commands) held fixed. --scenarios prints what the levers would give for several ASSUMED
speed-ups (nothing measured on the console yet: the console A/B is the test). --log: the last "[vgx] vitaGL:" line of a Vita3K or console log gives the
fractions of sceGxm calls skipped by MSC_VITA_VGL_SKIP.
"""
import csv, re, sys, statistics as st

COLS = ['glCalls', 'scenes', 'efbScenes', 'fboBinds', 'draws', 'drawIndices', 'programUses', 'uniformCalls', 'uniformBytes', 'palCalls', 'palBytes',
        'ringBytes', 'streamUploads', 'streamBytes', 'texBinds', 'texParams', 'vboBinds', 'attrPointers', 'attrToggles', 'attrConsts', 'layoutChanges',
        'scissorUpdates', 'stateCalls', 'fragSwitches', 'copyQuads', 'clears', 'mergeable', 'sameStateGap', 'mergedDraws', 'progSwitchOnly',
        'uniDupCalls', 'uniDupBytes', 'pairs', 'pairsSameProg', 'pairsDiffStatic', 'pairsDiffRs', 'pairsDiffTex', 'pairsDiffU0', 'pairsDiffU1', 'pairsDiffU2',
        'pairsDiffU3', 'pairsDiffU4', 'pairsDiffU5', 'streamSame1', 'streamSame3', 'streamAny1', 'idxBytes', 'idxSame1']

US_DRAW = 19.8e-3      # ms per draw command, net of the profiler (5 Oct console figure 26.5 us minus 6.7 us of clock read)
MS_PER_MB = 3.9        # uncached write, stream upload (console, 3.8-4.1)
NET_B_5OCT = 11.8      # 17.4 ms all= minus 5.6 ms of clock reads (docs/wave5-GLB.md 1); the 0.615 MB / 335 draws census of that window
# per-call savings ASSUMED for the skipped sceGxm calls (no measurement; libgxm calls are a few hundred ns to a few us each)
US_SKIP = {'vprog': 1.5e-3, 'fprog': 1.5e-3, 'stream': 0.4e-3, 'tex': 0.8e-3}

def load(path, min_draws):
    """A census csv (MSC_VGX_GLSTATS) or a game log (log.txt with MSC_VGX_PROF=1/2/3: its '[vgx] per frame:' lines are means over 100 frames each)."""
    if path.endswith('.txt'):
        rows = []
        for line in open(path, errors='replace'):
            if line.startswith('[vgx] per frame:'):
                r = {'frame': str(len(rows) + 1)}
                for kv in line.split(':', 1)[1].split():
                    k, _, v = kv.partition('=')
                    r[k] = v
                rows.append(r)
    else:
        rows = list(csv.DictReader(open(path)))
    op = [r for r in rows if float(r.get('draws', 0) or 0) > min_draws]
    return rows, op

def mean(op, k):
    return st.mean(float(r.get(k, 0) or 0) for r in op) if op else 0.0

def fixed_part():
    return NET_B_5OCT - 335 * US_DRAW - 0.615 * MS_PER_MB

def model(m):
    """Estimated net B per frame, ms [E]: console unit costs applied to the census; anchored so that the 5 Oct window gives the measured net 11.8 ms."""
    return m['draws'] * US_DRAW + m['streamBytes'] / 1e6 * MS_PER_MB + fixed_part()

def skip_fractions(log):
    """Last '[vgx] vitaGL:' line: skipped/calls vprog a/b fprog c/d stream e/f tex g/h -> per-draw call counts and skipped fractions."""
    last = None
    for line in open(log, errors='replace'):
        if '[vgx] vitaGL:' in line:
            last = line
    if not last:
        return None
    m = re.search(r'skipped/calls vprog (\d+)/(\d+) fprog (\d+)/(\d+) stream (\d+)/(\d+) tex (\d+)/(\d+)', last)
    if not m:
        return None
    v = list(map(int, m.groups()))
    names = ['vprog', 'fprog', 'stream', 'tex']
    out = {}
    for i, n in enumerate(names):
        sk, calls = v[2 * i], v[2 * i + 1]
        out[n] = (sk, calls)
    base = out['vprog'][1] or 1
    return {n: {'frac': (sk / calls if calls else 0.0), 'per_draw': calls / base} for n, (sk, calls) in out.items()}

def scenarios(m, sk):
    d = m['draws']
    s_mb = m['streamBytes'] / 1e6
    r_mb = m['ringBytes'] / 1e6
    base = model(m)
    print(f'estimated net B, open play, {d:.0f} draws, {s_mb:.2f} MB streamed, {r_mb:.2f} MB ring [E]')
    print(f'  baseline (uncached stream and ring, nothing skipped): {base:.2f} ms')
    print('  stream arena (the ring and the sets unchanged); the speed-up of the stream write is ASSUMED, not measured:')
    for f in (2, 4, 8):
        print(f'    stream written {f}x faster than uncached: -{s_mb * MS_PER_MB * (1 - 1 / f):.2f} ms -> {base - s_mb * MS_PER_MB * (1 - 1 / f):.2f} ms')
    print('  uniform ring in CDRAM (ASSUMED the same factors; the ring is part of the draw figure, 3.9 ms per MB assumed):')
    for f in (2, 4):
        print(f'    ring written {f}x faster: -{r_mb * MS_PER_MB * (1 - 1 / f):.2f} ms')
    if sk:
        tot = 0.0
        for n in ('vprog', 'fprog', 'stream', 'tex'):
            saved = d * sk[n]['per_draw'] * sk[n]['frac'] * US_SKIP[n]
            tot += saved
            print(f'  skipped {n}: {sk[n]["frac"] * 100:.0f} % of {sk[n]["per_draw"]:.1f} calls per draw x {US_SKIP[n] * 1000:.1f} us ASSUMED = -{saved:.2f} ms')
        print(f'    skipped sets together: -{tot:.2f} ms')
        a2, a4, a8 = (s_mb * MS_PER_MB * (1 - 1 / f) for f in (2, 4, 8))
        r2, r4 = (r_mb * MS_PER_MB * (1 - 1 / f) for f in (2, 4))
        print('  all three together:')
        print(f'    conservative (arena 2x, ring 2x, sets at half the assumed call cost): -{a2 + r2 + tot / 2:.2f} ms -> {base - a2 - r2 - tot / 2:.2f} ms')
        print(f'    middle       (arena 4x, ring 4x, sets as assumed):                    -{a4 + r4 + tot:.2f} ms -> {base - a4 - r4 - tot:.2f} ms')
        print(f'    optimistic   (arena 8x, ring 4x, sets as assumed):                    -{a8 + r4 + tot:.2f} ms -> {base - a8 - r4 - tot:.2f} ms')
        print('    no change if the faster memory is not faster (arena and ring at 1x): sets only -%.2f ms -> %.2f ms' % (tot, base - tot))
    else:
        print('  skipped sets: no --log with a "[vgx] vitaGL:" line')
    return base

def main():
    args = sys.argv[1:]
    min_draws = 250
    logs = []
    scen = False
    paths = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == '--min-draws':
            min_draws = int(args[i + 1]); i += 2
        elif a == '--log':
            logs.append(args[i + 1]); i += 2
        elif a == '--scenarios':
            scen = True; i += 1
        else:
            paths.append(a); i += 1
    runs = []
    for a in paths:
        rows, op = load(a, min_draws)
        runs.append((a, rows, op))
        print(f'{a}: {len(rows)} {"windows of 100 frames" if a.endswith(".txt") else "frames"}, {len(op)} open-play (draws > {min_draws}), {op[0]["frame"] if op else "-"}..{op[-1]["frame"] if op else "-"}')
    means = [{k: mean(op, k) for k in COLS} for _, _, op in runs]
    if scen:
        sk = skip_fractions(logs[0]) if logs else None
        scenarios(means[0], sk)
        return
    print(f'{"per open-play frame":<20}' + ''.join(f'{i:>14}' for i in range(len(runs))))
    for k in COLS:
        if any(m[k] for m in means):
            print(f'{k:<20}' + ''.join(f'{m[k]:>14.1f}' for m in means))
    print(f'{"est. net B ms [E]":<20}' + ''.join(f'{model(m):>14.2f}' for m in means))

main()
