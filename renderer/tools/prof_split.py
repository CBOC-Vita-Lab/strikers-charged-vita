#!/usr/bin/env python3
"""Split a qemu framecount-plugin profile into the layers of the graphics path.

  prof_split.py <tag> [frames-in-window=313] [top=25]      (files: build/host-gl-arm/runs/<tag>.{prof,bin,log})
  prof_split.py <profile> <arm-binary> <run-log> <frames> [top]

Game thread and other threads are reported separately, each split into
  translated game code | GX HLE layer (runtime/src/hle/gx) | vgx | xxhash (used by both) |
  other native runtime | libm | other libraries
Functions are attributed by name: a symbol defined in the hle/gx objects is "GX HLE", one defined
in the vgx objects (or in namespace vgx) is "vgx". Symbolisation follows tools/prof_report.py.
"""
import bisect, collections, glob, os, re, subprocess, sys

MSC = '/home/claude/msc'
RUNS = MSC + '/build/host-gl-arm/runs'
a = sys.argv[1:]
if len(a) <= 3 and not os.path.exists(a[0]):
    tag = a[0]
    prof, binary, log = '%s/%s.prof' % (RUNS, tag), '%s/%s.bin' % (RUNS, tag), '%s/%s.log' % (RUNS, tag)
    frames = float(a[1]) if len(a) > 1 else 313.0
    top = int(a[2]) if len(a) > 2 else 25
else:
    prof, binary, log, frames = a[0], a[1], a[2], float(a[3])
    top = int(a[4]) if len(a) > 4 else 25
NM = 'arm-linux-gnueabihf-nm'

def load_syms(path, dynamic=False):
    out = subprocess.run([NM, '-n', '--defined-only'] + (['-D'] if dynamic else []) + [path], capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 3 and p[1] in 'TtWwi':
            syms.append((int(p[0], 16) & ~1, p[2]))
    syms.sort()
    return [x for x, _ in syms], [n for _, n in syms]

def obj_names(patterns):
    names = set()
    for pat in patterns:
        for o in glob.glob(pat):
            out = subprocess.run([NM, '--defined-only', o], capture_output=True, text=True).stdout
            for line in out.splitlines():
                p = line.split()
                if len(p) >= 3 and p[1] in 'TtWw':
                    names.add(p[2])
    return names

# (every variant build directory: obj-null, obj-null-gx5, obj-glnull, ...)
hle = obj_names([MSC + '/build/host-gl-arm/obj-base/rt/runtime/src/hle/gx/*.o', MSC + '/build/host-gl-arm/obj-*null*/hle/*.o'])
vgx = obj_names([MSC + '/build/host-gl-arm/obj-*null*/vgx_*.o'])
# Inline functions of the runtime's headers (dispatch, memory access) are emitted into many
# objects; only names that no object outside hle/gx defines count as the HLE layer.
rest = obj_names([o for o in glob.glob(MSC + '/build/host-gl-arm/obj-base/rt/**/*.o', recursive=True) if '/hle/gx/' not in o])
hle -= rest
vgx -= rest

exe_addrs, exe_names = load_syms(binary)
hdr = subprocess.run(['arm-linux-gnueabihf-readelf', '-h', binary], capture_output=True, text=True).stdout
exe_is_pie = 'DYN' in hdr.split('Type:')[1].split('\n')[0]
maps, exe_map = [], None
for line in open(log, errors='replace'):
    m = re.match(r'\[maps\] ([0-9a-f]+)-([0-9a-f]+) \S+ ([0-9a-f]+) \S+ \S+\s+(\S+)', line)
    if not m: continue
    if m.group(4).startswith('/') and '.so' in m.group(4):
        maps.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16), m.group(4)))
    elif m.group(4).endswith('/' + os.path.basename(binary)) and exe_map is None:
        exe_map = (int(m.group(1), 16), int(m.group(3), 16))
libsyms = {}
def resolve(pc):
    for lo, hi, off, path in maps:
        if lo <= pc < hi:
            if path not in libsyms: libsyms[path] = load_syms(path, dynamic=True)
            addrs, names = libsyms[path]
            i = bisect.bisect_right(addrs, pc - lo + off) - 1
            return os.path.basename(path) + ':' + (names[i] if i >= 0 else '?')
    if exe_is_pie and exe_map: pc = pc - exe_map[0] + exe_map[1]
    i = bisect.bisect_right(exe_addrs, pc) - 1
    return exe_names[i] if i >= 0 else '?%x' % pc

def category(n):
    if re.match(r'func_[0-9A-F]{8}', n): return 'translated game code'
    if '.so' in n.split(':')[0]:
        return 'libm (software fma etc.)' if n.startswith('libm') else 'other libraries'
    if n.startswith('XXH') or 'XXH3' in n: return 'xxhash (HLE scan cache / vgx)'
    if n in vgx or n.startswith('_ZN3vgx') or n.startswith('_ZNK3vgx'): return 'vgx'
    if n in hle: return 'GX HLE layer'
    return 'other native runtime'

by = [collections.Counter(), collections.Counter()]
for line in open(prof):
    pc, d0, d1 = line.split()
    name = resolve(int(pc, 16))
    by[0][name] += int(d0); by[1][name] += int(d1)
def demangle(names):
    out = subprocess.run(['c++filt'], input='\n'.join(names), capture_output=True, text=True).stdout.splitlines()
    return dict(zip(names, out))
order = ['translated game code', 'libm (software fma etc.)', 'GX HLE layer', 'vgx', 'xxhash (HLE scan cache / vgx)', 'other native runtime', 'other libraries']
for idx, title in ((0, 'GAME THREAD'), (1, 'OTHER THREADS (render thread = vgx player; audio mixer)')):
    c = by[idx]; total = sum(c.values())
    if not total: continue
    cats = collections.Counter()
    for n, v in c.items(): cats[category(n)] += v
    print('== %s: %.2f M instructions/frame ==' % (title, total / frames / 1e6))
    for k in order:
        if cats[k]: print('   %8.2f M  %5.1f%%  %s' % (cats[k] / frames / 1e6, 100.0 * cats[k] / total, k))
    # PROF_ALL=1 in the environment: also the top functions of the runtime and library categories
    for k in (('GX HLE layer', 'vgx', 'xxhash (HLE scan cache / vgx)') + (('other native runtime', 'other libraries') if os.environ.get('PROF_ALL') else ())):
        fn = [(n, v) for n, v in c.most_common() if category(n) == k][:top]
        if not fn: continue
        dm = demangle([n for n, _ in fn])
        print('   -- %s: top functions --' % k)
        for n, v in fn:
            if v / frames < 5000: break
            print('   %8.3f M  %s' % (v / frames / 1e6, dm.get(n, n)[:120]))
