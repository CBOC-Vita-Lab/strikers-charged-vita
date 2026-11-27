#!/bin/bash
# The MSC_FPS=auto controller's unit test (platform/compat/np_fps_ctl.h): synthetic frame sequences, no game data, a few seconds.
#   tools/fps/run_fps_unit_test.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
out=${OUT:-${TMPDIR:-/tmp}/np-fps-unit}
mkdir -p "$out"
${CXX:-clang++} -std=c++11 -O1 -Iplatform/compat tools/fps/fps_unit.cpp tools/fps/gsplit_stub.cpp -o "$out/fps_unit"
"$out/fps_unit" | tail -4
"$out/fps_unit" | grep -q "^FAIL" && { echo "fps unit test: FAILED"; exit 1; }
${CXX:-clang++} -std=c++11 -O1 -Iplatform/compat tools/fps/fps_hold30_unit.cpp tools/fps/gsplit_stub.cpp -o "$out/fps_hold30_unit"
"$out/fps_hold30_unit"
${CXX:-clang++} -std=c++11 -O1 -D__vita__ -Iinclude -Iplatform/compat -Itools/fps/test_include '-DFPS_SOURCE="../../platform/compat/np_fps.cpp"' '-DPROBE_SOURCE="../../platform/vita/src/np_fps_probe.cpp"' tools/fps/fps_probe_unit.cpp tools/fps/gsplit_stub.cpp -o "$out/fps_probe_unit"
for case in gap failed-seed zero platform-registration; do env -i PATH="$PATH" LC_ALL=C "$out/fps_probe_unit" "$case"; done
# wave 18 (MSC_FPS_PROBE_EVERY): one synthetic game with the probe read every picture and once in 4 / 8: the 60 and the way back come within N (up) and 2N (down) pictures of the same decision, the reads are about 1/N, forced reads fire; mutants: no skipping (1), no forced reads (2)
P18=(-std=c++11 -O1 -D__vita__ -Iinclude -Iplatform/compat -Itools/fps/test_include '-DFPS_SOURCE="../../platform/compat/np_fps.cpp"' '-DPROBE_SOURCE="../../platform/vita/src/np_fps_probe.cpp"')
P18R() { env -i PATH="$PATH" LC_ALL=C MSC_FPS_PROBE_EVERY=$1 "$2" every 2>&1 | grep -a "^RESULT\|^FAIL" ; }
r1=$(P18R 1 "$out/fps_probe_unit"); r4=$(P18R 4 "$out/fps_probe_unit"); r8=$(P18R 8 "$out/fps_probe_unit")
for n in 4 8; do
  eval "r=\$r$n"
  case "$r" in *FAIL*) echo "MSC_FPS_PROBE_EVERY=$n unit: $r"; exit 1;; esac
  pa=$(echo "$r1" | sed 's/.*promote=\([0-9]*\).*/\1/'); pb=$(echo "$r" | sed 's/.*promote=\([0-9]*\).*/\1/')
  da=$(echo "$r1" | sed 's/.*down=\([0-9]*\).*/\1/'); db=$(echo "$r" | sed 's/.*down=\([0-9]*\).*/\1/')
  [ $pb -ge $pa ] && [ $pb -le $((pa+n)) ] && [ $db -ge $da ] && [ $db -le $((da+2*n)) ] || { echo "MSC_FPS_PROBE_EVERY=$n: decisions moved too far (60 at $pa vs $pb, back to 30 at $da vs $db)"; exit 1; }
done
echo "MSC_FPS_PROBE_EVERY unit: $(echo $r1 | cut -d' ' -f1-4) | $(echo $r4 | cut -d' ' -f1-4) | $(echo $r8 | cut -d' ' -f1-4)"
for b in 1 2; do ${CXX:-clang++} "${P18[@]}" -DNP_PROBE_EVERY_BREAK=$b tools/fps/fps_probe_unit.cpp tools/fps/gsplit_stub.cpp -o "$out/fps_probe_every_break" 2>/dev/null; rb=$(P18R 4 "$out/fps_probe_every_break" || true); echo "$rb" | grep -q "^FAIL" || { echo "MSC_FPS_PROBE_EVERY test: deliberate break $b was not caught"; exit 1; }; done
${CXX:-clang++} -std=c++11 -O1 -Iplatform/compat tools/fps/fps_lock60_unit.cpp tools/fps/gsplit_stub.cpp -o "$out/fps_lock60_unit"
"$out/fps_lock60_unit" > "$out/lock60.txt" || { tail -3 "$out/lock60.txt"; echo "LOCKED 60 controller test: FAILED"; exit 1; }
tail -1 "$out/lock60.txt"
for b in 1 2 3; do "$out/fps_lock60_unit" --break=$b > "$out/lock60_break.txt" || true; grep -q "^FAIL" "$out/lock60_break.txt" || { echo "LOCKED 60 controller test: deliberate break $b was not caught"; exit 1; }; done
${CXX:-clang++} -std=c++11 -O1 -Iplatform/compat tools/fps/cadence_unit.cpp tools/fps/gsplit_stub.cpp -o "$out/cadence_unit"
"$out/cadence_unit" > "$out/cadence.txt" || { grep "^FAIL" "$out/cadence.txt"; echo "MSC_CADENCE test: FAILED"; exit 1; }
tail -1 "$out/cadence.txt"
for b in 1 2 3 4; do "$out/cadence_unit" --break=$b > "$out/cadence_break.txt" || true; grep -q "^FAIL" "$out/cadence_break.txt" || { echo "MSC_CADENCE test: deliberate break $b was not caught"; exit 1; }; done
${CXX:-clang++} -std=c++11 -O1 -Iplatform/compat -I../renderer/src tools/fps/cadfull_unit.cpp tools/fps/gsplit_stub.cpp -o "$out/cadfull_unit"
"$out/cadfull_unit" > "$out/cadfull.txt" || { grep "^FAIL" "$out/cadfull.txt"; echo "MSC_CADENCE_FULL test: FAILED"; exit 1; }
tail -1 "$out/cadfull.txt"
for b in 1 2 3 4; do "$out/cadfull_unit" --break=$b > "$out/cadfull_break.txt" || true; grep -q "^FAIL" "$out/cadfull_break.txt" || { echo "MSC_CADENCE_FULL test: deliberate break $b was not caught"; exit 1; }; done
${CXX:-clang++} -std=c++11 -O1 -Iplatform/compat tools/fps/cad3_unit.cpp tools/fps/gsplit_stub.cpp -o "$out/cad3_unit"
"$out/cad3_unit" tools/fps/cad3_scenes.txt > "$out/cad3.txt" || { grep "^FAIL" "$out/cad3.txt"; echo "MSC_HELD_CADENCE test: FAILED"; exit 1; }
tail -1 "$out/cad3.txt"
for b in 1 2 3 4 5 6; do ${CXX:-clang++} -std=c++11 -O1 -Iplatform/compat -DNP_CAD3_BREAK=$b tools/fps/cad3_unit.cpp tools/fps/gsplit_stub.cpp -o "$out/cad3_break"; "$out/cad3_break" tools/fps/cad3_scenes.txt > "$out/cad3_break.txt" || true; grep -q "^FAIL" "$out/cad3_break.txt" || { echo "MSC_HELD_CADENCE test: deliberate break $b was not caught"; exit 1; }; done
${CXX:-clang++} -std=c++11 -O1 -Iinclude -Iplatform/compat tools/fps/cad3_wire_unit.cpp tools/fps/gsplit_stub.cpp -o "$out/cad3_wire_unit"
"$out/cad3_wire_unit" > "$out/cad3_wire.txt" 2>&1 || true; grep -aq "^cad3 wiring test: passed" "$out/cad3_wire.txt" || { echo "MSC_HELD_CADENCE wiring test: FAILED"; exit 1; }
${CXX:-clang++} -std=c++11 -O1 -Iinclude -Iplatform/compat -DNP_CAD3_BREAK=4 tools/fps/cad3_wire_unit.cpp tools/fps/gsplit_stub.cpp -o "$out/cad3_wire_break"
"$out/cad3_wire_break" > "$out/cad3_wire_break.txt" 2>&1 || true; grep -aq "^FAIL" "$out/cad3_wire_break.txt" || { echo "MSC_HELD_CADENCE wiring test: deliberate break was not caught"; exit 1; }
${CXX:-clang++} -std=c++11 -O1 -Iplatform/compat tools/fps/head17_unit.cpp tools/fps/gsplit_stub.cpp -o "$out/head17_unit"
"$out/head17_unit" | tail -1
for b in 1 2 3 4 5 6 7 8; do ${CXX:-clang++} -std=c++11 -O1 -Iplatform/compat -DNP_HEAD17_BREAK=$b tools/fps/head17_unit.cpp tools/fps/gsplit_stub.cpp -o "$out/head17_break"; "$out/head17_break" > "$out/head17_break.txt" || true; grep -q "^FAIL" "$out/head17_break.txt" || { echo "MSC_HEAD_STAT test: deliberate break $b was not caught"; exit 1; }; done
# pace18 (MSC_SIMT cost model, np_simpace18.h): modes 0/1 equal the old numbers bit for bit; overlap = max(window, step) + overhead; mutants 1-5
${CXX:-clang++} -std=c++11 -O1 -Iplatform/compat tools/fps/simpace18_unit.cpp tools/fps/gsplit_stub.cpp -o "$out/simpace18_unit"
"$out/simpace18_unit" | tail -1
for b in 1 2 3 4 5; do ${CXX:-clang++} -std=c++11 -O1 -Iplatform/compat -DNP_SIMPACE_BREAK=$b tools/fps/simpace18_unit.cpp tools/fps/gsplit_stub.cpp -o "$out/simpace18_break"; "$out/simpace18_break" > "$out/simpace18_break.txt" || true; grep -q "^FAIL" "$out/simpace18_break.txt" || { echo "simpace18 test: deliberate break $b was not caught"; exit 1; }; done
echo "fps unit test: passed"
# holdgpu24 (MSC_LOCK60_STICKY): the 60 / full-size-30 switching does not flap; mutants 1..4 (no minimum hold, the ordinary proof, no latch, a new scene forces 60) must each fail
${CXX:-clang++} -std=c++11 -O1 -Iplatform/compat tools/fps/fps_sticky_unit.cpp tools/fps/gsplit_stub.cpp -o "$out/fps_sticky_unit"
"$out/fps_sticky_unit" | tail -1
for b in 1 2 3 4; do ${CXX:-clang++} -std=c++11 -O1 -Iplatform/compat -DNP_STICKY_BREAK=$b tools/fps/fps_sticky_unit.cpp tools/fps/gsplit_stub.cpp -o "$out/fps_sticky_break"; "$out/fps_sticky_break" >/dev/null && { echo "MSC_LOCK60_STICKY test: deliberate break $b was not caught"; exit 1; }; done
echo "MSC_LOCK60_STICKY unit: mutants 1-4 caught"
# crowd25 (MSC_CROWD_GOV, np_crowdgov.h): CPU-bound load steps down, GPU-bound load does not touch the crowd, level 5 only under sustained overload, fast give-back, flapping load bounded; mutants 1..4 must each fail
${CXX:-clang++} -std=c++11 -O1 -Iplatform/compat tools/fps/crowdgov_unit.cpp tools/fps/gsplit_stub.cpp -o "$out/crowdgov_unit"
"$out/crowdgov_unit" | tail -1
for b in 1 2 3 4 5 6 7; do ${CXX:-clang++} -std=c++11 -O1 -Iplatform/compat -DNP_CROWDGOV_BREAK=$b tools/fps/crowdgov_unit.cpp tools/fps/gsplit_stub.cpp -o "$out/crowdgov_break"; "$out/crowdgov_break" >/dev/null && { echo "MSC_CROWD_GOV test: deliberate break $b was not caught"; exit 1; }; done
echo "MSC_CROWD_GOV unit: mutants 1-5 caught"
