#!/usr/bin/env python3
"""racereport.py: symbolise the output of the race-check plugin (libracecheck.so) of a run.

  racereport.py <executable> <err.log or run.log> [--top 60] [--benign]

Reads the "[racesum]" and "[race] kind=.. words=.. windows=.. game_pc=.. worker_pc=.. addr=.." lines (all of them from the plugin's
exit summary) and the run's "[maps]" lines (MSC_DUMP_MAPS=1), maps every pc to a function (the executable's symbol table; the shared
libraries' dynamic symbols by mapping offset) and the sample address to a region / global variable, then prints the groups
sorted by words, with the benign synchronisation groups (semaphores, the framesend module's own variables) marked.
Kinds: A game thread writes / worker reads, B both write, C game thread reads / worker writes."""
import re, sys, subprocess, bisect, argparse, collections, os
ap = argparse.ArgumentParser(); ap.add_argument('exe'); ap.add_argument('log'); ap.add_argument('--top', type=int, default=60); ap.add_argument('--benign', action='store_true')
a = ap.parse_args()
maps = []   # (start, end, perm, off, path)
race, summ = [], []
for line in open(a.log, errors='replace'):
    m = re.match(r'\[maps\] ([0-9a-f]+)-([0-9a-f]+) (\S+) ([0-9a-f]+) \S+ \d+\s+(\S.*)?', line)
    if m: maps.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3), int(m.group(4), 16), (m.group(5) or '').strip())); continue
    m = re.match(r'\[race\] kind=(\w) vcpu=(\d+)(\(worker\))? words=(\d+) windows=(\d+) game_pc=([0-9a-f]+) worker_pc=([0-9a-f]+) addr=([0-9a-f]+) frames=(-?\d+)-(-?\d+)', line)
    if m: race.append((m.group(1), int(m.group(4)), int(m.group(5)), int(m.group(6), 16), int(m.group(7), 16), int(m.group(8), 16), int(m.group(9)), int(m.group(10)), ('worker' if m.group(3) else 'cpu' + m.group(2)))); continue
    if line.startswith('[racesum]'): summ.append(line.strip())
for s in summ: print(s)
symcache = {}
def load_syms(path, dynamic):
    key = (path, dynamic)
    if key in symcache: return symcache[key]
    try:
        out = subprocess.run(['nm', '-C', '-S', '-n', '--defined-only'] + (['-D'] if dynamic else []) + [path], capture_output=True, text=True).stdout
    except Exception: out = ''
    addrs, names, sizes = [], [], []
    for l in out.splitlines():
        p = l.split(None, 3)
        if len(p) == 4 and re.match(r'[0-9a-f]+$', p[0]) and re.match(r'[0-9a-f]+$', p[1]):
            addrs.append(int(p[0], 16)); sizes.append(int(p[1], 16)); names.append(p[3])
        elif len(p) == 3 and re.match(r'[0-9a-f]+$', p[0]):
            addrs.append(int(p[0], 16)); sizes.append(0); names.append(p[2])
    symcache[key] = (addrs, sizes, names)
    return symcache[key]
def lookup(path, addr, dynamic):
    addrs, sizes, names = load_syms(path, dynamic)
    i = bisect.bisect_right(addrs, addr) - 1
    if i < 0: return None
    off = addr - addrs[i]
    return '%s+%#x' % (names[i], off) if off < 0x4000 else None
def where(addr, code):
    for (s, e, perm, off, path) in maps:
        if s <= addr < e:
            if path == '' or path.startswith('['):
                return 'anon %#x (%s)' % (addr, path or perm)
            if os.path.realpath(path) == os.path.realpath(a.exe):
                r = lookup(a.exe, addr, False)
                return r or ('exe %#x' % addr)
            base = min(x[0] - x[3] for x in maps if x[4] == path)   # load base = start - file offset of the first mapping
            r = lookup(path, addr - base, True)
            return '%s:%s' % (os.path.basename(path), r or '%#x' % (addr - base))
    if addr < 0x400000: 
        r = lookup(a.exe, addr, False)
        return r or ('%#x' % addr)
    return 'unmapped %#x' % addr
def region(addr):
    r = where(addr, False)
    if r.startswith('anon') and 0x80000000 <= addr < 0x81800000: return 'MEM1+%#x' % (addr - 0x80000000)
    if 0x90000000 <= addr < 0x94000000: return 'MEM2+%#x' % (addr - 0x90000000)
    if 0xfe000000 <= addr: return 'stack %#x' % addr
    return r
benign_words = ('sem_', '__new_sem', 'futex', 'NpFs', 'pthread', '__lll', 'np_sem')
groups = []
for (kind, words, windows, gpc, wpc, addr, f0, f1, who) in race:
    g, w, d = where(gpc, True), where(wpc, True), region(addr)
    ben = any(x in g + w + d for x in benign_words)
    groups.append((words, kind, windows, g, w, d, f0, f1, ben, who))
tot = sum(x[0] for x in groups); totb = sum(x[0] for x in groups if x[8])
print('%d groups, %d words (%d in synchronisation groups)' % (len(groups), tot, totb))
shown = 0
for words, kind, windows, g, w, d, f0, f1, ben, who in groups:
    if ben and not a.benign: continue
    print('%s %-6s %6d words %5d windows | game: %-46s | other: %-46s | at %s | frames %d-%d%s' % (kind, who, words, windows, g[:46], w[:46], d[:48], f0, f1, '  [sync]' if ben else ''))
    shown += 1
    if shown >= a.top: break
