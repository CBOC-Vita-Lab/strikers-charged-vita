#!/bin/bash
# check_display_stack.sh: the display-queue callback's stack (gxvita14, after Galaxy's tools/check_display_stack.py).
# sceGxm runs vitaGL's display callback (vglSetDisplayCallback(DisplayCallback), vita/vgx_host_vita.cpp) on a SYSTEM thread with a 4 KiB stack. A local buffer anywhere in that call
# chain (inlined helpers count) is paid on every picture, under newlib's printf path (~800 bytes) and the log wrapper. Galaxy lost a console to this twice.
# 1. DisplayCallback's own static frame <= FRAME_BUDGET (512; was 1152 before gxvita14).
# 2. The WORST CHAIN (display_stack_chain.py: the call graph from the relocations of every unit on it, frames from -fstack-usage) + a 512-byte system reserve must leave MARGIN (1024) of the 4096.
# Units: the host, the present model, the np pacing / latency / diag units, the log wrappers and sink, the boot screen and its console, the backend (VgxBackendLivePlay). Nothing is linked or kept.
#   VDEPS=<vitaGL prefix with include/>  VITASDK=/opt/vitasdk  [FRAME_BUDGET=512] [MARGIN=1024]  bash renderer/tools/check_display_stack.sh
# Exit 0 pass, 1 over budget, 2 could not compile / inconclusive. MAINTENANCE: a new callee of the callback in a unit not listed here shows up as UNKNOWN (exit 2): add its file to UNITS.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd); N=$(cd "$HERE/../native" && pwd)
VITASDK=${VITASDK:-/opt/vitasdk}; VDEPS=${VDEPS:-/out/v13-fast/gl/prefix}; FRAME_BUDGET=${FRAME_BUDGET:-512}; export MARGIN=${MARGIN:-1024}
VSYS=$VITASDK/arm-vita-eabi; GXX=15.2.0
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
COMMON="-std=gnu++20 -O2 -fno-math-errno -w -pipe --target=armv7a-none-eabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -fsigned-char -fshort-enums -femulated-tls -fno-pic -munaligned-access -fomit-frame-pointer -D__vita__ -D__USES_INITFINI__ -U_GNU_SOURCE -Wno-builtin-macro-redefined -nostdlibinc -isystem $VSYS/include/c++/$GXX -isystem $VSYS/include/c++/$GXX/arm-vita-eabi -isystem $VSYS/include/c++/$GXX/backward -isystem $VSYS/include -DTARGET_PC -DNOMINMAX -fstack-usage"
RF="-D__VGX_VITA__ -DVGX_NATIVE -DVITAGL_GLB -DMSC_VGX_COMMIT=\"stackcheck\" -I$HERE/vita/include -I$HERE/../dependencies/vitaGL/source -I$VDEPS/include -I$HERE/src -I$HERE/vita -I$HERE/native -I$HERE/native/vita_shim -I$HERE/include -I$HERE/third_party"
NF="-DNP_NATIVE=1 -DNP_VITA=1 -DNP_PLAYER=1 -DNP_LOGGING=1 -I$N/include -I$N/platform/vita/include -I$N/platform/vita/src -I$N/platform/compat -I$N/platform/sdk/include -I$N/platform/sdk/vita -I$N/platform/sdk/src -I$N/platform -I$N/src"
cc() { nice -n 15 clang++ $COMMON "$@" > "$T/log.$$" 2>&1 || { echo "check_display_stack: compile failed (INCONCLUSIVE): $*"; tail -4 "$T/log.$$"; exit 2; }; }
UNITS_R="vita/vgx_host_vita.cpp src/vgx_present.cpp native/vgx_native_vita_services.cpp src/vgx_backend_gl.cpp"
UNITS_N="platform/compat/np_fps.cpp platform/compat/np_latency.cpp platform/compat/np_touchdiag.cpp platform/compat/np_smooth.cpp platform/vita/src/vita_log.cpp platform/vita/src/vita_silent_stdio.cpp platform/vita/src/vita_bootscreen.cpp platform/vita/src/vita_console.cpp"
for f in $UNITS_R; do cc $RF -c "$HERE/$f" -o "$T/$(basename $f .cpp).o" & [ "$(jobs -r | wc -l)" -ge 2 ] && wait -n; done
for f in $UNITS_N; do cc $NF -c "$N/$f" -o "$T/$(basename $f .cpp).o" & [ "$(jobs -r | wc -l)" -ge 2 ] && wait -n; done
wait
# cc exits the subshell only; check the outputs
for f in $UNITS_R $UNITS_N; do [ -f "$T/$(basename $f .cpp).su" ] || { echo "check_display_stack: $f produced no .su (INCONCLUSIVE)"; exit 2; }; done
LINE=$(grep -E '_ZN12_GLOBAL__N_115DisplayCallbackEPv' "$T/vgx_host_vita.su" | head -1)
[ -n "$LINE" ] || { echo "check_display_stack: DisplayCallback not found (renamed? INCONCLUSIVE)"; exit 2; }
BYTES=$(echo "$LINE" | awk -F'\t' '{print $2}'); KIND=$(echo "$LINE" | awk -F'\t' '{print $3}')
echo "DisplayCallback's own frame (GateDisplay, PaceDisplayNote inlined): $BYTES bytes, $KIND; budget $FRAME_BUDGET"
RC=0
{ [ "$KIND" = static ] && [ "$BYTES" -le "$FRAME_BUDGET" ]; } || { echo "FAIL: own frame over budget or dynamic"; RC=1; }
python3 "$HERE/tools/display_stack_chain.py" "$T"; C=$?
[ $C -eq 2 ] && exit 2
[ $C -ne 0 ] && RC=1
[ $RC -eq 0 ] && echo PASS || echo FAIL
exit $RC
