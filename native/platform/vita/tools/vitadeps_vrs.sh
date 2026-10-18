#!/bin/bash
# vitaGL build profiles for the Vita package (workstream VRS, wave 4). One heavy turn:
#   tools/heavy.sh -t 590 platform/vita/tools/vitadeps_vrs.sh <profile> [OUT]
# builds only libvitaGL.a (the recipe is one/vita/Makefile.vitadeps, cdbba42 + the 8 MiB uniform-ring patch) with the options of the
# profile into OUT (default build/np/VRS/deps-<profile>) and takes the other libraries and headers (mathneon, vitashark,
# SceShaccCgExt, taihen stub) from the strict build the packages used so far (they do not depend on the profile).
#   strict  what kit-e1df95a was built with: shader cache, shark log, error log, depth/stencil store, no splash, STRICT_DRAW_COMPLIANCE
#   perf    VRS: no STRICT_DRAW_COMPLIANCE, DRAW_SPEEDHACK (one stream per attribute, no client arrays, which vgx never uses),
#           SKIP_ERROR_HANDLING (upstream NO_DEBUG), HAVE_SCRATCH_MEMORY (vglSetupScratchMemory / the circular pool serves GL_STREAM_DRAW buffers);
#           the error log and the shark log stay (a first console run needs them)
#   speed   perf without LOG_ERRORS and HAVE_SHARK_LOG (the player package once the logs are known to be clean)
set -eu
export VITASDK=/opt/vitasdk PATH=/opt/vitasdk/bin:$PATH
MSC=/home/claude/msc
P=${1:?profile: strict|perf|speed}
OUT=${2:-$MSC/build/np/VRS/deps-$P}
BASE="-DHAVE_SHADER_CACHE -DSTORE_DEPTH_STENCIL -DSKIP_SPLASHSCREEN"
case $P in
  strict) OPTS="$BASE -DHAVE_SHARK_LOG -DLOG_ERRORS -DSTRICT_DRAW_COMPLIANCE";;
  perf)   OPTS="$BASE -DHAVE_SHARK_LOG -DLOG_ERRORS -DHAVE_SCRATCH_MEMORY -DSKIP_ERROR_HANDLING -DDRAW_SPEEDHACK";;
  speed)  OPTS="$BASE -DHAVE_SCRATCH_MEMORY -DSKIP_ERROR_HANDLING -DDRAW_SPEEDHACK";;
  *) echo "unknown profile $P"; exit 2;;
esac
SRC=$MSC/build/one/vdeps-strict
mkdir -p $OUT/lib $OUT/include
make -f $MSC/one/vita/Makefile.vitadeps DEPS_OUT=$OUT VGL_OPTS="$OPTS" -j2 $OUT/lib/libvitaGL.a headers
for f in libSceShaccCgExt.a libmathneon.a libtaihen_stub.a libvitashark.a; do cp -n $SRC/lib/$f $OUT/lib/; done
cp -n $SRC/include/* $OUT/include/ 2>/dev/null || true
cat $OUT/vitagl/options.txt
