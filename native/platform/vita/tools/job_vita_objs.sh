#!/bin/bash
# Compiles every game object for the Vita (VitaSDK GCC 15.2, NPB's Makefile, ABI contract flags) in slices that
# fit one heavy.sh turn each (the make is incremental: a cut slice resumes where it stopped).
#   platform/vita/tools/job_vita_objs.sh [make vars...]     (runs `heavy.sh -t 540 make ... objs` until it is complete)
# Output: /home/claude/msc/build/np/NPV3/vita/obj (BUILD_ROOT=...), log: $BUILD_ROOT/job-vita-objs.log
cd "$(dirname "$0")/../../.."
export VITASDK=/opt/vitasdk PATH=/opt/vitasdk/bin:$PATH
BR=${BUILD_ROOT:-/home/claude/msc/build/np/NPV3}
VARS=(BUILD_ROOT=$BR GX_VGX=${GX_VGX:-/home/claude/msc/npw/vgx-npv3} WITH_SDK=1 WITH_GX=1 "OPT=-O2 -ffunction-sections -fdata-sections" DEBUG=-g0 "$@")
while :; do
  /home/claude/msc/tools/heavy.sh -t 540 make -k -j2 TARGET=vita "${VARS[@]}" objs
  rc=$?
  echo "== slice rc=$rc $(date +%T)"
  [ $rc = 124 ] && continue
  break
done
left=$(make -n TARGET=vita "${VARS[@]}" objs 2>/dev/null | grep -c 'arm-vita-eabi-')
echo "== objects still to build: $left"
