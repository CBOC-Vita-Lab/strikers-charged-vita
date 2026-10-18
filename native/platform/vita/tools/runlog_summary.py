#!/usr/bin/env python3
"""Summarises the log.txt of a run of the native port (Vita3K or the console): timeline of scenes and match states with the real clock (rt) and the
frame number, speed of the game's clock against real time, frame-interval windows, shader work, memory peaks, how the run ended.
    python3 runlog_summary.py log.txt [--every N]
Works on the [nav] / [watch] / [frame-timing] / [mem] / [script] / [vgx-vita] lines the harness and the platform layer write (np_harness.cpp, np_frame_timing.h)."""
import re, sys

def main(path, every=1):
    scenes = []; watch = []; fts = []; mems = []; script = []; end = []; shader = None; surface = []
    for line in open(path, errors='replace'):
        line = line.rstrip('\n')
        if line.startswith('[watch]'):
            kv = dict(re.findall(r'(\w+)=(\S+)', line)); watch.append(kv)
        elif line.startswith('[nav]'):
            scenes.append(line)
        elif line.startswith('[script]'):
            script.append(line)
        elif line.startswith('[frame-timing]') and 'reason=window' in line or line.startswith('[frame-timing]') and 'reason=identity' not in line:
            fts.append(dict(re.findall(r'(\w+)=(\S+)', line)))
        elif line.startswith('[mem]'):
            mems.append(line)
        elif 'shader work so far' in line:
            shader = line
        elif 'renderer start-up' in line or 'full screen:' in line or 'MSC_VITA_RENDER' in line:
            surface.append(line)
        elif re.search(r'run ended|RUN ENDED|CRASH|STALL|fatal|FATAL|reached MSC_MAX_FRAMES|script exit|\[script\] .*(TIMEOUT|exit)', line):
            end.append(line)
    print('== start-up'); [print('  ' + l[:200]) for l in surface]
    print('== script / state timeline'); [print('  ' + l[:160]) for l in script if 'STATE' in l or 'TIMEOUT' in l or 'end' in l or 'first run' in l or 'confirmed' in l]
    print('== watch (every %d): f  rt  ret  scene  gs  clock  score  sim' % every)
    prev = None; prevclk = 0.0
    for i, w in enumerate(watch):
        if i % every: continue
        rt = float(w.get('rt', 0)); f = int(w.get('f', 0)); ret = int(w.get('ret', 0))
        fps = ''
        if prev:
            clk = float(w.get('clk', '0/0').split('/')[0]) if w.get('clk') else 0.0
            mc = ' match clock %.2fx real' % ((clk - prevclk) / max(rt - prev[1], 1e-9)) if clk > 0 and prevclk > 0 else ''
            fps = ' %5.1f fps%s' % ((f - prev[0]) / max(rt - prev[1], 1e-9), mc)
        prev = (f, rt, ret); prevclk = float(w.get('clk', '0/0').split('/')[0]) if w.get('clk') else 0.0
        print('  f=%-6d rt=%-8.1f ret=%-7d %-34s gs=%-3s clk=%-10s score=%-6s sim=%s%s' % (f, rt, ret, w.get('scene', '')[:34], w.get('gs'), w.get('clk'), w.get('score'), w.get('sim'), fps))
    if fts:
        print('== frame-interval windows (us): first_frame n p50 p95 max over_33333')
        for t in fts[:60]: print('  %s n=%s p50=%s p95=%s max=%s over33=%s scene=%s gs=%s' % (t.get('first_frame'), t.get('n'), t.get('p50_us'), t.get('p95_us'), t.get('max_us'), t.get('over_33333'), t.get('scene'), t.get('gs')))
    if shader: print('== ' + shader[:200])
    if mems:
        print('== memory (last report)'); print('  ' + mems[-1][:400])
    print('== end'); [print('  ' + l[:220]) for l in end[-8:]]
    if watch: print('  last watch: f=%s rt=%s scene=%s gs=%s clk=%s score=%s' % (watch[-1].get('f'), watch[-1].get('rt'), watch[-1].get('scene'), watch[-1].get('gs'), watch[-1].get('clk'), watch[-1].get('score')))

if __name__ == '__main__':
    a = sys.argv[1:]
    every = 1
    if '--every' in a:
        i = a.index('--every'); every = int(a[i + 1]); del a[i:i + 2]
    main(a[0], every)
