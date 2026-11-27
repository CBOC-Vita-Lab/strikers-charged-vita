#!/bin/bash
# MSC_REDUCED_MOTION: a quarter of the camera shake, the random draws and the length of the effect unchanged.   tools/cam/run_reduced_motion_test.sh [outdir]
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-/tmp/np_reduced_motion}
mkdir -p "$OUT"
clang++ -m32 -msse2 -mfpmath=sse -isystem /usr/include/x86_64-linux-gnu -DNP_TARGET_HOST32=1 -DNP_NATIVE=1 -DNP_DISC_HOST_ORDER=1 -DBUILD_VERSION=0 -DVERSION_R4QE01 -DNDEBUG=1 -DdNODEBUG=1 -DdSINGLE=1 \
  -Iplatform/gx/include -I. -Iinclude -Isrc/ode -Isrc/zlib -Ilibs/RVL_SDK/include -Iref/RVL_SDK -Ilibs/Runtime/include -idirafter libs/MSL_C/include -idirafter libs/MetroTRK/include \
  -fno-strict-aliasing -fwrapv -fno-short-enums -fshort-wchar -w -include np/compat.h -std=gnu++11 -fno-exceptions -fno-rtti -Wno-c++11-narrowing -O1 -Iplatform/sdk/include -funsigned-char \
  tools/cam/test_reduced_motion.cpp src/Game/Camera/noisefilter.cpp src/Game/Camera/rumblefilter.cpp -o "$OUT/test_reduced_motion"
A=$(env -u MSC_REDUCED_MOTION "$OUT/test_reduced_motion"); B=$(MSC_REDUCED_MOTION=1 "$OUT/test_reduced_motion"); C=$(MSC_REDUCED_MOTION=0 "$OUT/test_reduced_motion")
echo "off:  $A"; echo "on:   $B"
python3 - "$A" "$B" "$C" <<'PY'
import sys,re
def parse(s): return {k: v for k, v in (t.split('=') for t in s.replace('rumble=','rumble=').split())}
a,b,c=[parse(x) for x in sys.argv[1:4]]
ok=True
def chk(cond,what):
    global ok
    print(('ok   ' if cond else 'FAIL ')+what); ok&=bool(cond)
chk(a==c,'MSC_REDUCED_MOTION=0 is the same as unset')
chk(a['draws']==b['draws'] and a['frames']==b['frames'],'same random draws and same length of the shake')
chk(abs(float(b['peak'])-0.25*float(a['peak']))<1e-5 and abs(float(b['sum'])-0.25*float(a['sum']))<1e-4,'noise displacement is a quarter')
ra=[float(x) for x in a['rumble'].split(',')]; rb=[float(x) for x in b['rumble'].split(',')]
chk(all(abs(y-0.25*x)<1e-6 for x,y in zip(ra,rb)) and a['ks']==b['ks'] and a['kd']==b['kd'],'rumble start offset is a quarter, spring and damping unchanged')
sys.exit(0 if ok else 1)
PY
