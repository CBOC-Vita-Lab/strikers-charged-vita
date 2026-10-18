#!/bin/bash
# SKIN (W5): Vita lab + player packages of the W5SKIN worktrees, the W5INT way (int_build.sh stage 3 / job_vita_final.sh: ThinLTO, glb vitaGL),
# one heavy turn, resumable:   tools/heavy.sh -t 1500 tools/skin/vita.sh
#   lab     $R/lto/vita/pkg/np/mscharged-np.{vpk,elf,eboot.bin}            MSCN00001
#   player  $R/lto/vita/pkg-player/np/mscharged-np.{vpk,elf,eboot.bin}     MSCN00003, shader seed SEED (default: W5SKIN seed if made, else W5SEED seed-core)
cd /home/claude/msc/npw/W5SKIN || exit 1
export VITASDK=/opt/vitasdk PATH=/opt/vitasdk/bin:$PATH VDEPS=/home/claude/msc/build/np/W5GLB/deps-glb
R=/home/claude/msc/build/np/W5SKIN
GX=/home/claude/msc/npw/W5SKIN-vgx
SEED=${SEED:-$([ -d $R/seed-skin/shadercache ] && echo $R/seed-skin/shadercache || echo /home/claude/msc/build/np/W5SEED/seed-core/shadercache)}
mk() { local root=$1; shift; make -j2 TARGET=vita BUILD_ROOT=$root GX_VGX=$GX VGX=$GX WITH_SDK=1 WITH_GX=1 "OPT=-O2 -ffunction-sections -fdata-sections" DEBUG=-g0 "$@" 2>&1 | grep -a -v "^/opt/vitasdk\|^clang\|^arm-vita\|^make\[\|^mkdir\|wchar_t yet\|32-bit enums\|GNU-stack\|NOTE: This\|^env \|^  NP_GAME" | tail -${TAILN:-6}; return ${PIPESTATUS[0]}; }
echo "== skin vita $(date +%T) np $(git rev-parse --short HEAD)$(git diff --quiet || echo +dirty) vgx $(git -C $GX rev-parse --short HEAD)$(git -C $GX diff --quiet || echo +dirty) seed $SEED"
rm -rf $R/lto/vita/pkg/np/obj $R/lto/vita/pkg-player/np/obj   # (the lab build id is a compile-time define of the platform objects)
echo "== lab (LTO) $(date +%T)";    mk $R/lto LTO=thin vpk || exit 1
echo "== player (LTO) $(date +%T)"; mk $R/lto LTO=thin NP_PLAYER=1 VPK_OUT=$R/lto/vita/pkg-player NP_SHADER_SEED=$SEED vpk || exit 1
ls -la $R/lto/vita/pkg/np/*.vpk $R/lto/vita/pkg-player/np/*.vpk
for v in $R/lto/vita/pkg/np/mscharged-np.vpk $R/lto/vita/pkg-player/np/mscharged-np.vpk; do unzip -l $v | grep -ci shacccg | sed "s|^|libshacccg entries in $(basename $(dirname $(dirname $v))): |"; done
echo "== skin vita done $(date +%T)"
