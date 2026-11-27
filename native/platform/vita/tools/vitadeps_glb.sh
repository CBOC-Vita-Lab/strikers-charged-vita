#!/bin/bash
# vitaGL "glb" profile (workstream GLB, wave 5): the perf profile (docs/wave4-VRS.md) plus the GLB patch series in platform/vita/vitagl/.
#   tools/heavy.sh -t 590 platform/vita/tools/vitadeps_glb.sh [OUT]
# copies vitaGL's source (upstream 9a14d17, taken from a full clone of Rinnegatamante/vitaGL; the first version of this script used the older cdbba42) to OUT/vitagl-src, applies the patches 0001-0008 in order, and builds only libvitaGL.a with
# one/vita/Makefile.vitadeps (VGL= the patched copy, the same 8 MiB uniform ring patch); the other libraries and headers come from the strict build.
# Same options as perf (HAVE_SCRATCH_MEMORY, DRAW_SPEEDHACK, SKIP_ERROR_HANDLING, the error and shark logs). The new behaviour is switched at run time
# by vgx (vita/vgx_host_vita.cpp, MSC_VITA_VGL_STREAM_KIB / _FLUSH / _SKIP) and is OFF by default; the fixed pool alignment and the statistics are always on.
set -eu
export VITASDK=/opt/vitasdk PATH=/opt/vitasdk/bin:$PATH
MSC=/work/msc
NP=$(cd "$(dirname "$(readlink -f "$0")")/../../.." && pwd)
REF=${VGL_REF:?set VGL_REF to a full clone of https://github.com/Rinnegatamante/vitaGL}
UPSTREAM_BASE=9bf3e3f4e55efd1f4a191b179144f7f9a98cd925   # (1.2: was 9a14d17; the patch series is 0001-0029)
OUT=${1:-$MSC/build/np/W5GLB/deps-glb}
SRC=$OUT/vitagl-src
rm -rf "$SRC"; mkdir -p "$SRC" "$OUT/lib" "$OUT/include"
git -C "$REF" archive "$UPSTREAM_BASE" source | tar -x -C "$SRC"
for p in "$NP"/platform/vita/vitagl/*.patch; do
  echo "patch $(basename "$p")"
  patch -s -p1 -d "$SRC" < "$p"
done
BASE="-DHAVE_SHADER_CACHE -DSTORE_DEPTH_STENCIL -DSKIP_SPLASHSCREEN"
OPTS="$BASE -DHAVE_SHARK_LOG -DLOG_ERRORS -DHAVE_SCRATCH_MEMORY -DSKIP_ERROR_HANDLING -DDRAW_SPEEDHACK"
make -f $MSC/one/vita/Makefile.vitadeps DEPS_OUT=$OUT VGL=$SRC VGL_HASH=9a14d17-glb VGL_OPTS="$OPTS" -j2 $OUT/lib/libvitaGL.a headers
S=$MSC/build/one/vdeps-strict
for f in libSceShaccCgExt.a libmathneon.a libtaihen_stub.a libvitashark.a; do cp -n $S/lib/$f $OUT/lib/; done
cp -n $S/include/* $OUT/include/ 2>/dev/null || true
cat $OUT/vitagl/options.txt
