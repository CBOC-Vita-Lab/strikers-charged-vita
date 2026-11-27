#!/bin/sh
# Host tests of np/overlap.h (x86-64 g++ with ASan and UBSan; the Vita build itself is not compiled here). Run from anywhere.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
NATIVE=$(cd "$HERE/../.." && pwd)
OUT=${OUT:-$(mktemp -d)}
GAME="-DNP_NATIVE=1 -DNP_DISC_HOST_ORDER=1 -DBUILD_VERSION=0 -DVERSION_R4QE01 -DNDEBUG=1 -I$NATIVE/platform/gx/include -I$NATIVE -I$NATIVE/include -I$NATIVE/libs/RVL_SDK/include -I$NATIVE/ref/RVL_SDK -I$NATIVE/libs/Runtime/include -idirafter $NATIVE/libs/MSL_C/include -fno-strict-aliasing -fwrapv -fno-short-enums -w -funsigned-char -fpermissive -Wno-narrowing -include np/compat.h"
SAN="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -ffunction-sections -fdata-sections"
g++ -std=gnu++11 -fno-exceptions -fno-rtti $GAME $SAN -c "$NATIVE/src/NL/gl/glRenderList.cpp" -o "$OUT/glRenderList.o"
g++ -std=gnu++11 -fno-exceptions -fno-rtti $GAME $SAN -c "$HERE/test_snaponce.cpp" -o "$OUT/test_snaponce.o"
g++ $SAN -Wl,--gc-sections "$OUT/test_snaponce.o" "$OUT/glRenderList.o" -o "$OUT/test_snaponce"
"$OUT/test_snaponce"
T_ONCE=1 "$OUT/test_snaponce"
g++ -std=gnu++17 -I"$NATIVE/include" -DNP_NATIVE=1 $SAN "$HERE/test_switches.cpp" "$NATIVE/platform/compat/np_overlap.cpp" -o "$OUT/test_switches_host"
g++ -std=gnu++17 -I"$NATIVE/include" -DNP_NATIVE=1 -DNP_OVERLAP_VITA_DEFAULTS $SAN "$HERE/test_switches.cpp" "$NATIVE/platform/compat/np_overlap.cpp" -o "$OUT/test_switches_vita"
"$OUT/test_switches_host" host
"$OUT/test_switches_vita" vita
for v in MSC_FS_SNAPONCE=0 MSC_POSE_CULL=0 MSC_CROWD_POSEONCE=0 MSC_CROWD_REFRESH=1 MSC_NPC_LAZY=0; do env $v "$OUT/test_switches_vita" "$v"; done
env MSC_POSE_CULL=2 MSC_CROWD_REFRESH=3 "$OUT/test_switches_host" pose2
python3 "$HERE/crowd_schedule_model.py"
