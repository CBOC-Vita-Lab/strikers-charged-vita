#!/usr/bin/env python3
"""1.3 pack top-up: the warm-up list of a pack that serves MSC_VGX_PALMERGE=2.

  pal_list.py --base last/vgx_shaderlist.bin --catalogue cat.shl --rec DIR [--rec DIR ...] --top N -o vgx_shaderlist.bin

The last pack's list stays as it is (the warm-up stops compiling when the C heap reaches MSC_VGX_WARM_HEAP_MIB, default 44 MiB, and the 1.3 list of 242 shaders is
already close to it: a tiers.py list of 270 shaders was cut at 251 in Vita3K), and the N 60-row palette vertex keys (perVtxPos 2, palRows 60) that the most match-like
recordings under the --rec folders used are added as keys alone. The mode arms itself on the first such key and uses a twin only if its key is listed; the twins' programs link
from the pack on first use. N=21 (the keys 14 or more of 256 runs used) fitted the limit, 34 did not. Every key added must be in the catalogue (= in the pack).
"""
import argparse, glob, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shl

ap = argparse.ArgumentParser()
ap.add_argument('--base', required=True); ap.add_argument('--catalogue', required=True)
ap.add_argument('--rec', action='append', required=True); ap.add_argument('--top', type=int, required=True); ap.add_argument('-o', required=True)
ap.add_argument('--match', default=r'^(ai_|hu_|cap_|cheat_|dm_|mt_|pause_|tour|match60|items|replay|mega|item_)')
a = ap.parse_args()
ma = re.compile(a.match); cat = shl.read(a.catalogue); cs = set(cat.vs); cnt = {}
for d in a.rec:
    for f in glob.glob(os.path.join(d, '*.shl')):
        if ma.match(os.path.basename(f)):
            for k in shl.read(f).vs:
                if k[shl.VS_PERVTXPOS] == 2 and k[shl.VS_PALROWS] == 60: cnt[k] = cnt.get(k, 0) + 1
L = shl.merge([shl.read(a.base)])
for k, c in sorted(cnt.items(), key=lambda x: -x[1])[:a.top]:
    if k not in cs: sys.exit('a twin key is not in the catalogue')
    L.add_vs(k)
L.dups = 0
shl.write(L, a.o)
print('%s: %d twin keys of %d used; vs %d fs %d prog %d var %d' % ((a.o, min(a.top, len(cnt)), len(cnt)) + shl.counts(L)))
