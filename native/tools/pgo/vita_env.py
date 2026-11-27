#!/usr/bin/env python3
"""vita_env.py [vita_plan.cpp]: NAME=VALUE lines of the switches the Vita build turns on by itself (kSpeed and kQuality of vita_plan.cpp),
plus the 60 Hz thread layout of the console, for PGO_ENV of record.sh and for the armhf proxy runs. The host harness does not run vita_plan.cpp,
so a profile recorded without these would skip the code paths the defaults select (FASTIDX kernels, SKINMEMO, culling, cadence...)."""
import re, sys, os
here = os.path.dirname(os.path.abspath(__file__))
src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, '..', '..', 'platform', 'vita', 'src', 'vita_plan.cpp')
t = open(src).read()
t = re.sub(r'//[^\n]*', '', t)
out = {}
for arr in ('kSpeed', 'kQuality'):
    m = re.search(arr + r'\[\]\[2\]\s*=\s*\{(.*?)\};', t, re.S)
    for k, v in re.findall(r'\{"(MSC_[A-Z0-9_]+)",\s*"([^"]*)"\}', m.group(1)):
        out[k] = v
# policy and pacing switches that read the real clock or the console's display, and the Vita-only ones: not meaningful under qemu's virtual time
skip = ('MSC_VITA_', 'MSC_SLIP', 'MSC_PACE_VBLANK', 'MSC_CADENCE', 'MSC_PRESENT_SYNC_START', 'MSC_FPS_LADDERHOLD', 'MSC_DYNRES_', 'MSC_PIC_PREWARM',
        'MSC_POOL_', 'MSC_HITCH_MS')
out = {k: v for k, v in out.items() if not k.startswith(skip)}
out.update({'MSC_FPS': '60', 'MSC_FRAMESEND': 'thread', 'MSC_VGX_BACKEND_THREAD': '1', 'MSC_FS_PIPE': '1'})
for k, v in out.items():
    print('%s=%s' % (k, v))
