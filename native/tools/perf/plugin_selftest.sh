#!/bin/bash
# NPP2: a few seconds under qemu-arm: the race-check plugin on the frame-send unit test (armhf), thread mode with marks. The test's own
# counters (g_checks) are written by both sides inside the windows, so the plugin must report kind-B groups, and the run must pass.
#   tools/heavy.sh -t 120 tools/perf/plugin_selftest.sh        (also run at the start of job_fs.sh)
B=/home/claude/msc/build/np/NPP2
cd /home/claude/msc/npw/NPP2 || exit 1
make -C platform/vita TARGET=armhf OUT=$B/vita-host $B/vita-host/armhf/test_framesend 2>&1 | grep -E "error|Error" | head -3
BIN=$B/vita-host/armhf/test_framesend
[ -x $BIN ] || { echo "plugin selftest: no armhf test binary"; exit 1; }
MSC_FRAMESEND=thread MSC_FS_MARKS=1 /home/claude/research/qemu-full/build/qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf \
  -plugin $PWD/tools/perf/libracecheck.so -d plugin $BIN > $B/plugin_selftest.log 2>&1
echo "plugin selftest rc=$?  $(grep -a -c '^\[race\]' $B/plugin_selftest.log) race groups; $(grep -a '^\[racesum\]' $B/plugin_selftest.log | cut -c1-160)"
grep -a "checks," $B/plugin_selftest.log | tail -1
grep -a '^\[race\] kind=B' $B/plugin_selftest.log | head -3
