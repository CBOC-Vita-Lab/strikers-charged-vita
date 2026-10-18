#!/bin/bash
# W5 GLB: the glb console packages (lab + player), resumable; run as: tools/heavy.sh -t 590 platform/vita/tools/glb_packages.sh   (after vitadeps_glb.sh; R=build/np/W5GLB: edit the paths for another root)
#   stage 0: seed $R/lto from W4 (cp -a, fix .d paths; W4 itself is never written)
#   stage 1: lab build MSCN00001    -> $R/lto/vita/pkg/np/mscharged-np.vpk        (libvitaGL = deps-glb, vgx = W5GLB-vgx)
#   stage 2: player MSCN00003+seed  -> $R/lto/vita/pkg-player/np/mscharged-np.vpk
cd /home/claude/msc/npw/W5GLB
export VITASDK=/opt/vitasdk PATH=/opt/vitasdk/bin:$PATH
R=/home/claude/msc/build/np/W5GLB
GX=/home/claude/msc/npw/W5GLB-vgx
export VDEPS=${VDEPS:-/home/claude/msc/build/np/W5GLB/deps-glb}
SEED=$R/seed/shadercache
# The shader seed of W4 was made with the perf library (cache key 30223cc9). The glb library has another id string (cdbba42-glb), so another key; the compiled shaders are the
# same (the patch touches no shader code), so the seed folder is copied under the glb key (the key is the FNV-1a of "<options.txt>|<kHostShaderAbi>|gen <n>", as ShaderCacheFolder computes).
mkseed() {
  [ -f $VDEPS/vitagl/options.txt ] || return 0
  rm -rf $SEED; mkdir -p $SEED; cp -a /home/claude/msc/build/np/W4/seed/shadercache/. $SEED/
  K=$(python3 -I - "$VDEPS/vitagl/options.txt" <<'PY'
import sys
vid = open(sys.argv[1]).read().rstrip("\n")
i = "%s|%s|gen %u" % (vid, "tex0-7 on TEXCOORD0-7, col0/1 on TEXCOORD8/9, uv on TEXCOORD0; mode GLOBAL; 1", 2)
h = 2166136261
for c in i.encode():
    h = ((h ^ c) * 16777619) & 0xffffffff
print("%08x" % h)
PY
)
  [ -d $SEED/$K ] || cp -a $SEED/30223cc9 $SEED/$K
  echo "== shader seed: perf key 30223cc9 copied to the glb key $K (kHostShaderAbi tex0-7..1, gen 2)"
}
[ -f $VDEPS/lib/libvitaGL.a ] || { echo "no $VDEPS/lib/libvitaGL.a (run platform/vita/tools/vitadeps_glb.sh first)"; exit 1; }
if [ ! -d $R/lto ]; then
  echo "== seeding $R/lto from W4 (cp -a, fix .d paths)"
  cp -a /home/claude/msc/build/np/W4/lto $R/lto || exit 1
  find $R/lto -name '*.d' -print0 | xargs -0 sed -i 's|/build/np/W4/|/build/np/W5GLB/|g'
  # the game sources of this worktree are newer than W4's objects but identical (W5GLB changes no game source): objects must not be rebuilt
  find $R/lto/vita/obj $R/lto/vita/pobj -type f -exec touch {} +
fi
mk() { local root=$1; shift; make -j2 TARGET=vita BUILD_ROOT=$root GX_VGX=$GX WITH_SDK=1 WITH_GX=1 "OPT=-O2 -ffunction-sections -fdata-sections" DEBUG=-g0 "$@" 2>&1 | grep -a -v "^/opt/vitasdk\|^clang\|^arm-vita\|^make\[\|^mkdir\|wchar_t yet\|32-bit enums\|GNU-stack\|NOTE: This\|^env \|^  NP_GAME" | tail -${TAILN:-12}; return ${PIPESTATUS[0]}; }
mkseed
echo "== glb $(date +%T) np $(git rev-parse --short HEAD) vgx $(git -C $GX rev-parse --short HEAD) VDEPS=$VDEPS"
rm -rf $R/lto/vita/pkg/np/obj $R/lto/vita/pkg-player/np/obj
# the seeded W4 executables are NOT up to date for this build (other vgx, other libvitaGL): force the link
rm -f $R/lto/vita/pkg/np/mscharged-np.elf $R/lto/vita/pkg/np/mscharged-np.velf $R/lto/vita/pkg/np/mscharged-np.eboot.bin $R/lto/vita/pkg/np/mscharged-np.vpk $R/lto/vita/pkg-player/np/mscharged-np.elf $R/lto/vita/pkg-player/np/mscharged-np.velf $R/lto/vita/pkg-player/np/mscharged-np.eboot.bin $R/lto/vita/pkg-player/np/mscharged-np.vpk
echo "== lab (LTO) $(date +%T)";    mk $R/lto LTO=thin vpk || exit 1
echo "== player (LTO) $(date +%T)"; mk $R/lto LTO=thin NP_PLAYER=1 VPK_OUT=$R/lto/vita/pkg-player NP_SHADER_SEED=$SEED vpk || exit 1
for e in $R/lto/vita/pkg/np/mscharged-np.elf $R/lto/vita/pkg-player/np/mscharged-np.elf; do echo "$e: MSC_VITA_VGL_STREAM_KIB strings=$(strings -n 8 $e 2>/dev/null | grep -c MSC_VITA_VGL_STREAM_KIB) vglGlbGetStats=$(/opt/vitasdk/bin/arm-vita-eabi-nm $e 2>/dev/null | grep -c vglGlbGetStats)"; done
ls -la $R/lto/vita/pkg/np/*.vpk $R/lto/vita/pkg-player/np/*.vpk 2>/dev/null
echo "== glb done $(date +%T)"
