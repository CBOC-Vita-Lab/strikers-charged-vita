#!/bin/bash
# FPS job D (ONE heavy slot, heavy.sh -t 3600): the Vita lab package of the W5FPS worktree (ThinLTO, vgx w5fps), then two Vita3K runs of the scripted match (real time):
#   v3k-auto  MSC_FPS=auto      v3k-30  default (MSC_FPS=30)       both with MSC_FPS_LOG=1; the log shows [vi] (the retrace follows the emulator's vblank: its measured rate),
#   [vgx-vita] display lines (how many vblanks each picture was shown for) and, in auto, the [fps] auto: switches.
# Output: build/np/W5FPS/lto/vita/pkg/np/mscharged-np.vpk, build/np/W5FPS/v3k-runs/<tag>/{log.txt,run.txt,...}. Never packs or logs the owner's libshacccg.suprx (the emulator profile links it).
NP=${NP:-/home/claude/msc/npw/W5FPS}
R=${R:-/home/claude/msc/build/np/W5FPS}
T00=$(date +%s)
export VITASDK=/opt/vitasdk PATH=/opt/vitasdk/bin:$PATH
export GX_VGX=${GX_VGX:-/home/claude/msc/npw/W5FPS-vgx} VDEPS=/home/claude/msc/build/np/W5GLB/deps-glb
cd $NP || exit 1
VPK=$R/lto/vita/pkg/np/mscharged-np.vpk
echo "== vita lab build (LTO) $(date +%T) np $(git rev-parse --short HEAD) vgx $(git -C $GX_VGX rev-parse --short HEAD)"
make -j2 TARGET=vita BUILD_ROOT=$R/lto GX_VGX=$GX_VGX WITH_SDK=1 WITH_GX=1 "OPT=-O2 -ffunction-sections -fdata-sections" DEBUG=-g0 LTO=thin vpk > $R/vita-build.log 2>&1
echo "== vita build rc=$? $(date +%T) after $(( $(date +%s) - T00 )) s: $(ls -la $VPK 2>&1 | cut -c25-)"
[ -f $VPK ] || { grep -a -E "error|Error" $R/vita-build.log | head -20; exit 1; }
for t in auto 30; do
  [ $(( $(date +%s) - T00 )) -gt 2700 ] && { echo "v3k-$t: SKIPPED (budget)"; continue; }
  FILES="platform/vita/session/nav/match60.txt" NOHEAVY=1 RUNS_DIR=$R/v3k-runs PROFILE_DIR=$R/v3k \
    platform/vita/tools/job_v3k.sh v3k-$t 700 $NP/tools/fps/env/v3k-$t.txt "script end|CRASH|STALL" $VPK MSCN00001 > $R/v3k-$t.out 2>&1
  echo "v3k-$t: $(tail -3 $R/v3k-$t.out | tr '\n' ' ' | cut -c1-300)"
done
echo "== jobD done $(date +%T) after $(( $(date +%s) - T00 )) s"
