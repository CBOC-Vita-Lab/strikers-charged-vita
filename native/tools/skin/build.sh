#!/bin/bash
# SKIN (W5): build an executable of the np worktree W5SKIN with the vgx worktree W5SKIN-vgx (one heavy turn; make is incremental, resumable).
#   tools/heavy.sh -t 1500 tools/skin/build.sh h32null|h32gl|armhf
# Outputs under /home/claude/msc/build/np/W5SKIN/<target>/ (np-host32-full-null, np-host32-full, np-armhf-full).
NP=/home/claude/msc/npw/W5SKIN; R=${SKINR:-/home/claude/msc/build/np/W5SKIN}; VGX=${GX_VGX:-/home/claude/msc/npw/W5SKIN-vgx}
cd $NP || exit 1
case "$1" in
  h32null) A="TARGET=host32 OPT=-O1 EXESUF=-full-null GX_BACKEND=null";;
  h32gl)   A="TARGET=host32 OPT=-O1 EXESUF=-full GX_BACKEND=gl";;
  armhf)   A="TARGET=armhf OPT=-O2 EXESUF=-full GX_ARM_BACKEND=glnull";;
  *) echo "usage: build.sh h32null|h32gl|armhf"; exit 2;;
esac
echo "== skin build $1 np $(git rev-parse --short HEAD)$(git diff --quiet || echo +dirty) vgx $(git -C $VGX rev-parse --short HEAD)$(git -C $VGX diff --quiet || echo +dirty) $(date +%T)"
make -k -j2 $A BUILD_ROOT=$R GX_VGX=$VGX WITH_SDK=1 WITH_GX=1 WITH_PLAT=1 DEBUG=-g0 ${EXTRA:-} > $R/build-$1.log 2>&1
rc=$?
grep -a -E "error:|Error [0-9]" $R/build-$1.log | head -20
echo "== skin build $1 rc=$rc $(date +%T)"; ls -la $R/*/np-*-full* 2>/dev/null
exit $rc
