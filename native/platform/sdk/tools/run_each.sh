#!/bin/bash
# Runs every unit test of a target on its own (a failure does not stop the others) and prints a summary:
#   tools/run_each.sh host32|armhf [timeout_seconds_per_test]
# armhf runs under qemu-arm (cortex-a9); call it through /home/claude/msc/tools/heavy.sh.
T=${1:?target}
TMO=${2:-120}
HERE=$(cd "$(dirname "$0")/.." && pwd)
B=${B:-/home/claude/msc/build/np/NPS/$T}
RUN=""
[ "$T" = armhf ] && RUN="/home/claude/research/qemu-full/build/qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf"
export NP_TEST_RUNNER="$RUN"   # tests that start themselves again (test_virtual)
make -C "$HERE" -j2 T=$T $(for t in "$HERE"/tests/test_*.cpp; do echo "$B/tests/$(basename "$t" .cpp)"; done) "$B/tests/test_thp_player" >/dev/null 2>&1 || echo "build errors (see make output)"
pass=0; fail=0; failed=""
for t in "$B"/tests/test_*; do
  case "$t" in *.o) continue;; esac
  [ -x "$t" ] || continue
  name=$(basename "$t")
  echo "=== $name"
  start=$(date +%s.%N)
  timeout "$TMO" $RUN "$t" 2>&1 | grep -v '^\[os\]\|^\[dvd\] no sys\|^\[nand\]'
  rc=${PIPESTATUS[0]}
  el=$(echo "$(date +%s.%N) - $start" | bc)
  if [ "$rc" = 0 ]; then pass=$((pass+1)); echo "--- $name: pass (${el}s)"; else fail=$((fail+1)); failed="$failed $name(rc=$rc)"; echo "--- $name: FAIL rc=$rc (${el}s)"; fi
done
echo "SUMMARY $T: $pass passed, $fail failed$failed"
[ $fail = 0 ]
