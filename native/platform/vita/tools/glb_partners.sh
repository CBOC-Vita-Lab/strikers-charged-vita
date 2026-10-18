#!/bin/bash
# W5 GLB: the A/B partners of the glb package from the SAME sources with the strict and the perf vitaGL library (lab packages only): tools/heavy.sh -t 900 platform/vita/tools/glb_partners.sh   (in the build root of the glb package: it rebuilds the two vgx objects that see vitaGL.h, so run it in a root nobody else builds in)
# The vgx objects that see vitaGL.h (the GL back end, the Vita host) and the library are rebuilt for each; the result goes to pkgs/<variant>/lab.{vpk,elf}. The glb packages are in pkgs/glb (built by glb_j1.sh).
cd /home/claude/msc/npw/W5GLB
export VITASDK=/opt/vitasdk PATH=/opt/vitasdk/bin:$PATH
R=/home/claude/msc/build/np/W5GLB
GX=/home/claude/msc/npw/W5GLB-vgx
V=$R/lto/vita/gx/vgx/vita-vitagl
for v in strict perf; do
  case $v in strict) export VDEPS=/home/claude/msc/build/one/vdeps-strict;; perf) export VDEPS=/home/claude/msc/build/np/VRS/deps-perf;; esac
  echo "== $v $(date +%T) VDEPS=$VDEPS"
  rm -f $V/obj/src/vgx_backend_gl.o $V/obj/vita/vgx_host_vita.o $V/libvgx-native-vita.a
  rm -rf $R/lto/vita/pkg/np/obj
  rm -f $R/lto/vita/pkg/np/mscharged-np.elf $R/lto/vita/pkg/np/mscharged-np.velf $R/lto/vita/pkg/np/mscharged-np.eboot.bin $R/lto/vita/pkg/np/mscharged-np.vpk
  make -j2 TARGET=vita BUILD_ROOT=$R/lto GX_VGX=$GX WITH_SDK=1 WITH_GX=1 "OPT=-O2 -ffunction-sections -fdata-sections" DEBUG=-g0 LTO=thin vpk 2>&1 | grep -a -v "^/opt/vitasdk\|^clang\|^arm-vita\|^make\[\|^mkdir\|wchar_t yet\|32-bit enums\|GNU-stack\|NOTE: This\|^env \|^  NP_GAME" | tail -4
  E=$R/lto/vita/pkg/np/mscharged-np.elf
  [ -f $E ] || { echo "$v: no elf"; exit 1; }
  mkdir -p $R/pkgs/$v && cp -p $E $R/pkgs/$v/lab.elf && cp -p $R/lto/vita/pkg/np/mscharged-np.vpk $R/pkgs/$v/lab.vpk && cp -p $R/lto/vita/pkg/np/mscharged-np.eboot.bin $R/pkgs/$v/lab.eboot.bin
  echo "$v: vglGlbGetStats symbols=$(arm-vita-eabi-nm $E 2>/dev/null | grep -c vglGlbGetStats) (0 expected) VGL id: $(strings -n 8 $E | grep -m1 '^vitaGL [0-9a-f]* built with' | cut -c1-90)"
  ls -la $R/pkgs/$v/lab.vpk
done
echo "== j6 done $(date +%T)"
