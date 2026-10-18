#!/bin/bash
# The final package set of a session, in one heavy turn (incremental when the game objects are up to date):
#   lab build (ThinLTO)      $R/lto/vita/pkg/np/mscharged-np.{vpk,elf,eboot.bin}       MSCN00001
#   player build (ThinLTO)   $R/lto/vita/pkg-player/np/...                             MSCN00003, with the shader seed of the Vita3K profile when there is one
#   -O2 flavour (no LTO)     $R/c1/vita/pkg/np/...                                    the A/B partner
#   platform smoke           $R/c1/vita/pkg/np/np-smoke.{vpk,eboot.bin}
#   kit                      $R/kit-<np hash>   (a directory of its own: the kit of an earlier turn, which a session may be using, is never touched)
#   tools/heavy.sh -t 590 platform/vita/tools/job_vita_final.sh        (resumable: run again after a timeout)
cd "$(dirname "$0")/../../.."
export VITASDK=/opt/vitasdk PATH=/opt/vitasdk/bin:$PATH
R=${R:-/home/claude/msc/build/np/NPV3}
GX=${GX_VGX:-/home/claude/msc/npw/vgx-npv3}
MK="make -j2 TARGET=vita GX_VGX=$GX WITH_SDK=1 WITH_GX=1 OPT=-O2\ -ffunction-sections\ -fdata-sections DEBUG=-g0"
mk() { local root=$1; shift; make -j2 TARGET=vita BUILD_ROOT=$root GX_VGX=$GX WITH_SDK=1 WITH_GX=1 "OPT=-O2 -ffunction-sections -fdata-sections" DEBUG=-g0 "$@" 2>&1 | grep -a -v "^/opt/vitasdk\|^clang\|^arm-vita\|^make\[\|^mkdir\|wchar_t yet\|32-bit enums\|GNU-stack\|NOTE: This\|^env \|^  NP_GAME" | tail -${TAILN:-6}; return ${PIPESTATUS[0]}; }
echo "== final $(date +%T) np $(git rev-parse --short HEAD)"
# the shader seed: what the emulator's last session compiled (vitaGL's cache of that profile)
SEEDSRC=$R/v3k/pref/ux0/data/mscharged/shadercache
SEED=$R/seed/shadercache
if [ -d "$SEEDSRC" ] && [ "$(find "$SEEDSRC" -name '*.gxp' | wc -l)" -gt 0 ]; then rm -rf "$R/seed"; mkdir -p "$R/seed"; cp -a "$SEEDSRC" "$SEED"; echo "seed: $(find $SEED -name '*.gxp' | wc -l) shaders, $(du -sk $SEED | cut -f1) KiB"; fi
# fresh platform objects: the lab build id (git hash + time) is a compile-time define of every one of them
rm -rf $R/lto/vita/pkg/np/obj $R/lto/vita/pkg-player/np/obj $R/c1/vita/pkg/np/obj
echo "== lab (LTO) $(date +%T)";    mk $R/lto LTO=thin vpk || exit 1
echo "== player (LTO) $(date +%T)"; mk $R/lto LTO=thin NP_PLAYER=1 VPK_OUT=$R/lto/vita/pkg-player ${SEED:+NP_SHADER_SEED=$SEED} vpk || exit 1
echo "== -O2 (no LTO) $(date +%T)"; mk $R/c1 vpk || exit 1
echo "== smoke $(date +%T)"; make -f platform/vita/Makefile.vpk smoke NP_VBUILD=$R/c1/vita/pkg 2>&1 | tail -3
cp $R/c1/vita/pkg/np/np-smoke.eboot.bin $R/lto/vita/pkg/np/ 2>/dev/null
{ echo "ThinLTO: clang -flto=thin -O2 bitcode + VitaSDK GNU ld with LLVM's gold plugin (platform/vita/Makefile.vpk NP_CLANG_LTO); glxSwap.cpp is an ordinary object"; } > $R/lto/vita/pkg/ltoinfo.txt
echo "== kit $(date +%T)"
KITDIR=${KITDIR:-$R/kit-$(git rev-parse --short HEAD)}
O2PKG=$R/c1/vita/pkg/np platform/vita/session/make_kit.sh -p $R/lto/vita/pkg-player/np $R/lto/vita/pkg/np $KITDIR | tail -12
ls -la $R/lto/vita/pkg/np/*.vpk $R/lto/vita/pkg-player/np/*.vpk $R/c1/vita/pkg/np/*.vpk
echo "== final done $(date +%T)"
