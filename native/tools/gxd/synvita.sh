#!/bin/bash
# GXD: Vita-flags syntax check of vgx sources (the exact clang++ command of `make -n TARGET=vita`, with -fsyntax-only); light, no object files.
#   tools/gxd/synvita.sh <vgx dir> [file ...]
D=${1:?vgx dir}; shift
FILES=("$@"); [ ${#FILES[@]} = 0 ] && FILES=(vgx_rec vgx_play vgx_state vgx_draw vgx_dlcache vgx_texcache vgx_guestmem vgx_stats vgx_interp vgx_api)
SRC=${SRC:-/home/claude/msc/npw/W5GXD}
cd $SRC || exit 1
make -n TARGET=vita BUILD_ROOT=/home/claude/msc/build/np/W5GXD/vitadry GX_VGX=$D WITH_SDK=1 WITH_GX=1 OPT=-O2 2>/dev/null > /tmp/synvita.$$.mk
rc=0
for f in "${FILES[@]}"; do
  f=${f%.cpp}
  cmd=$(grep -E "^clang\+\+ .* -c $D/src/$f\.cpp -o " /tmp/synvita.$$.mk | head -1)
  [ -n "$cmd" ] || { echo "no vita compile line for $f"; rc=1; continue; }
  cmd=$(echo "$cmd" | sed -E "s# -c $D/src/$f\.cpp -o [^ ]+# -fsyntax-only -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers $D/src/$f.cpp#; s# -w -pipe# -pipe#")
  echo "== $f"; [ -n "$SHOWCMD" ] && echo "$cmd" | cut -c1-300
  eval nice -n 15 $cmd 2>&1 | grep -v "^In file included" | head -${HEADN:-20}
  [ ${PIPESTATUS[0]} = 0 ] || rc=1
done
rm -f /tmp/synvita.$$.mk
exit $rc
