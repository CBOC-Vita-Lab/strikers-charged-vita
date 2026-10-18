#!/usr/bin/env python3
"""prof_native.py: hot functions of the NATIVE game from a framecount2 block profile (qemu-arm run, run.sh profile).

  prof_native.py <prof.txt> <run.log> <exe> <frames> [--steps N] [--map exe.map] [--top 40] [--cats]

prof.txt lines: "pc insns_game_thread insns_other_threads" (framecount2.c: game thread = the vCPU that issues the frame marks).
Run addresses are mapped to link addresses with the [maps] line of run.log (MSC_DUMP_MAPS=1) and the ELF load segments (exact file offset of the PC; works for PIE and non-PIE).
--map is the linker map (-Wl,-Map, see tools/perf/build.sh): every PC is attributed to the object file whose .text input section
holds it, and the objects are grouped (game NL / Game / ode / SDK layer / glue+vgx / platform / libc ...). Without LTO this is the
real split; with LTO the objects are merged, so only the function names remain.
Numbers are executed ARM instructions per presented frame (and per simulation step with --steps), not time."""
import argparse, bisect, collections, re, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument('prof'); ap.add_argument('log'); ap.add_argument('exe'); ap.add_argument('frames', type=float)
ap.add_argument('--steps', type=float, default=0); ap.add_argument('--map'); ap.add_argument('--top', type=int, default=40)
ap.add_argument('--thread', choices=['game', 'other'], default='game')
a = ap.parse_args()
NM, RE = 'arm-linux-gnueabihf-nm', 'arm-linux-gnueabihf-readelf'

def syms_of(path, dynamic=False):
    out = subprocess.run([NM, '-n', '-S', '--defined-only'] + (['-D'] if dynamic else []) + [path], capture_output=True, text=True).stdout
    s = []
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 4 and p[2] in 'TtWwi':
            s.append((int(p[0], 16) & ~1, p[3], int(p[1], 16)))
        elif len(p) == 3 and p[1] in 'TtWwi':
            s.append((int(p[0], 16) & ~1, p[2], 0))
    s.sort()
    return [x for x, _, _ in s], [n for _, n, _ in s], [z for _, _, z in s]

exe_addrs, exe_names, _exe_sizes = syms_of(a.exe)
# rebasing
maps = []; exe_maps = []
for line in open(a.log, errors='replace'):
    m = re.match(r'\[maps\] ([0-9a-f]+)-([0-9a-f]+) \S+ ([0-9a-f]+) \S+ \S+\s+(\S+)', line)
    if not m: continue
    lo, hi, off, path = int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16), m.group(4)
    if path.endswith(a.exe.rsplit('/', 1)[-1]): exe_maps.append((lo, hi, off))
    elif path.startswith('/') and '.so' in path: maps.append((lo, hi, off, path))
loads = []
for line in subprocess.run([RE, '-lW', a.exe], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if p and p[0] == 'LOAD': loads.append((int(p[1], 16), int(p[2], 16), int(p[4], 16)))   # off, vaddr, filesz
is_exec = 'EXEC (Executable' in subprocess.run([RE, '-h', a.exe], capture_output=True, text=True).stdout   # non-PIE (NOPIC=1) builds: run address = link address
def exe_vaddr(pc):
    # file offset of the PC, then the ELF load segment whose file range holds it (a page can lie in two segments,
    # so the page-aligned offset is ambiguous: use the exact offset).  Non-PIE and PIE both work.
    for lo, hi, off in exe_maps:
        if lo <= pc < hi:
            fo = pc - lo + off
            for foff, va, fsz in loads:
                if foff <= fo < foff + fsz: return va + fo - foff
            return None
    return None
libsyms = {}
def resolve(pc):
    for lo, hi, off, path in maps:
        if lo <= pc < hi:
            if path not in libsyms: libsyms[path] = syms_of(path, True)
            ad, nm, sz = libsyms[path]
            q = pc - lo + off
            i = bisect.bisect_right(ad, q) - 1
            lib = path.rsplit('/', 1)[-1]
            if i >= 0 and (sz[i] == 0 and q - ad[i] < 0x400 or ad[i] <= q < ad[i] + sz[i]): return lib + ':' + nm[i], None
            return lib + ':(local %x)' % (q & ~0xff), None
    v = exe_vaddr(pc)
    if v is None: return '?%x' % pc, None
    i = bisect.bisect_right(exe_addrs, v) - 1
    return (exe_names[i] if i >= 0 else '?%x' % v), v

# link map: input .text sections -> object
secs = []
if a.map:
    cur = None
    for line in open(a.map, errors='replace'):
        m = re.match(r'^ (\.text\S*)\s*$', line)
        if m: cur = m.group(1); continue
        m = re.match(r'^ (\.text\S*)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$', line)
        if m: secs.append((int(m.group(2), 16), int(m.group(3), 16), m.group(4).strip())); cur = None; continue
        m = re.match(r'^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$', line)
        if m and cur:
            secs.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3).strip())); cur = None
        elif not line.startswith(' '): cur = None
    secs.sort()
sec_addrs = [s[0] for s in secs]
def obj_of(v):
    i = bisect.bisect_right(sec_addrs, v) - 1
    if i >= 0 and secs[i][0] <= v < secs[i][0] + secs[i][1]: return secs[i][2]
    return None
CATS = [(r'/obj/src/NL/', 'game: NL'), (r'/obj/src/Game/GL', 'game: Game/GL'), (r'/obj/src/Game/', 'game: Game'), (r'/obj/src/ode', 'game: ode'),
        (r'/obj/src/', 'game: other src'), (r'/pobj/platform/compat|/pobj/ref/', 'compat'), (r'libnpsdk\.a\((ax_|dsp_hle|audio_out|AX|mix\.c|DSPCode)', 'SDK: audio (AX mixer, DSP HLE)'),
        (r'libnpsdk\.a\((thp_native|THPSimple)', 'SDK: THP movie'), (r'libnpsdk\.a\((os_|np_rt|np_plat|OS)', 'SDK: OS / virtual time'),
        (r'libnpsdk\.a\((input_native|np_menu|np_touch|np_game_input)', 'SDK: input'), (r'libnpsdk\.a\(mtx_native', 'SDK: MTX math'),
        (r'libnpsdk\.a\((dvd_|nand_|vi_native)', 'SDK: DVD / NAND / VI'), (r'libnpsdk', 'SDK layer: other'),
        (r'libnpgx|/gx/obj', 'GX glue (NPR)'), (r'libvgx|/vgx/', 'vgx'), (r'libnpplat|/plat/', 'platform/harness (NPV)'),
        (r'libnpstubs', 'stubs'), (r'libc|libm|libgcc|libstdc|crt', 'libc/libm/libgcc')]
def category(obj, name):
    if obj:
        for pat, c in CATS:
            if re.search(pat, obj): return c
        return 'other: ' + obj.rsplit('/', 1)[-1]
    if ':' in name: return 'shared lib: ' + name.split(':')[0]
    return 'unattributed'

by = [collections.Counter(), collections.Counter()]
cat = [collections.Counter(), collections.Counter()]
fname = {}
for line in open(a.prof):
    pc, d0, d1 = line.split(); pc = int(pc, 16)
    name, v = resolve(pc)
    c = category(obj_of(v) if (v is not None and secs) else None, name) if secs else None
    for k, d in ((0, int(d0)), (1, int(d1))):
        if d:
            by[k][name] += d
            if c: cat[k][c] += d
idx = 0 if a.thread == 'game' else 1
tot = sum(by[idx].values())
den = a.frames
print(f"== {a.thread.upper()} THREAD(S): {tot/den/1e6:.3f} M instructions per presented frame over {a.frames:.0f} frames"
      + (f" ({tot/a.steps/1e6:.3f} M per simulation step, {a.steps:.0f} steps)" if a.steps else ''))
if secs:
    print('-- by object group (linker map)')
    for k, v in cat[idx].most_common(): print(f"   {v/den/1e6:8.3f} M  {100*v/tot:5.1f}%  {k}")
dm = subprocess.run(['c++filt'], input='\n'.join(n for n, _ in by[idx].most_common(a.top)), capture_output=True, text=True).stdout.splitlines()
print(f'-- top {a.top} functions')
acc = 0
for (n, v), d in zip(by[idx].most_common(a.top), dm):
    acc += v
    print(f"   {v/den/1e6:8.3f} M  {100*v/tot:5.1f}%  cum {100*acc/tot:5.1f}%  {d[:120]}")
