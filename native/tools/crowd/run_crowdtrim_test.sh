#!/bin/bash
# MSC_VGX_CROWDTRIM (the crowd's sprite quads cut to the box a figure was drawn in): the parts that need no emulator.
#   - the switch is read by the renderer, is in the Vita player build's speed list (on), and has its A/B runs in the console kit
#   - the kit files are what matrix.py gives, and the kit's own tests pass for the runs that name the switch
# Optional, with a host harness run script: RUNB="<script that takes TAG SCRIPT FRAMES DUMPLIST [ENV=VALUE...] and leaves TAG/pics/frame_*.png>" ROUTE=<input script>
# also runs the open-play frames 2300 and 2450 with the switch off and on (mode 2, which reads the pictures back and checks every cut quad) and compares the PNGs.
set -e
root=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$root"
fail() { echo "FAIL: $*"; exit 1; }
grep -q 'EnvInt("MSC_VGX_CROWDTRIM"' renderer/src/vgx_backend_gl.cpp || fail "the renderer does not read MSC_VGX_CROWDTRIM"
grep -q '{"MSC_VGX_CROWDTRIM", "1"}' native/platform/vita/src/vita_plan.cpp || fail "MSC_VGX_CROWDTRIM is not in the player build's speed list"
grep -q 'run("ctrim0"' tools/console-1.2/matrix.py || fail "the kit has no ctrim0 run"
grep -q 'run("lookctrim0"' tools/console-1.2/matrix.py || fail "the kit has no lookctrim0 run (tier 1)"
grep -q 'MSC_VGX_CROWDTRIM=' docs/TECHNICAL.md || fail "docs/TECHNICAL.md does not describe MSC_VGX_CROWDTRIM"
python3 tools/console-1.2/gen_kit.py --check || fail "the console kit files differ from matrix.py (run gen_kit.py)"
if [ -n "$RUNB" ] && [ -n "$ROUTE" ]; then
  tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
  L=MSC_VGX_LEAD=1000000   # (frames away from a dump frame are drawn too: the cells of the crowd's pictures are known)
  "$RUNB" ct-off "$ROUTE" 2452 2300,2450 $L > "$tmp/off.log" 2>&1
  "$RUNB" ct-on "$ROUTE" 2452 2300,2450 $L MSC_VGX_CROWDTRIM=2 MSC_VGX_CROWDTRIM_CHECK=2300,2450 > "$tmp/on.log" 2>&1
  out=${HARNESS_OUT:-$(dirname "$RUNB")}
  for f in 002300 002450; do
    a=$(md5sum "$out/ct-off/pics/frame_$f.png" | cut -d' ' -f1)
    b=$(md5sum "$out/ct-on/pics/frame_$f.png" | cut -d' ' -f1)
    [ "$a" = "$b" ] || fail "frame $f differs with MSC_VGX_CROWDTRIM=2"
  done
  if grep -q 'VIOLATION' "$out/ct-on/log.txt"; then fail "a picture had texels outside its measured box"; fi
  grep -q 'crowdtrim-check' "$out/ct-on/log.txt" || fail "the check did not run"
fi
echo "crowdtrim test ok"
