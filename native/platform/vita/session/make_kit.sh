#!/bin/bash
# Assembles the console kit for the coordinator (who pushes it to the console from the Mac):
#   platform/vita/session/make_kit.sh [-p <player pkg dir>] [<lab pkg dir> [<kit dir>]]
# lab pkg dir  = where `make TARGET=vita ... vpk` put mscharged-np.* (default build/np/NPV3/c1/vita/pkg/np); np-smoke.eboot.bin is taken from there if present
# player dir   = the same for a build made with NP_PLAYER=1 (title MSCN00003, no lab); optional
# kit dir      = build/np/NPV3/kit
# The kit holds no game data; the executables are our own build of the port, which stays on this machine like the sources.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
NP=$(cd "$HERE/../../.." && pwd)
PLAYER=""
if [ "${1:-}" = -p ]; then PLAYER=$2; shift 2; fi
PKG=${1:-/home/claude/msc/build/np/NPV3/c1/vita/pkg/np}
KIT=${2:-/home/claude/msc/build/np/NPV3/kit}
rm -rf "$KIT"; mkdir -p "$KIT/env" "$KIT/nav" "$KIT/nav-native"
cp "$PKG/mscharged-np.eboot.bin" "$KIT/eboot-np-lab.bin"
cp "$PKG/mscharged-np.vpk" "$KIT/mscharged-np.vpk"
# the extracted application directories vitacompanion's `promote` takes (a VPK is a zip file: eboot.bin, sce_sys/...)
mkdir -p "$KIT/app-MSCN00001" && unzip -q -o "$PKG/mscharged-np.vpk" -d "$KIT/app-MSCN00001"
if [ -n "$PLAYER" ]; then
  mkdir -p "$KIT/app-MSCN00003" && unzip -q -o "$PLAYER/mscharged-np.vpk" -d "$KIT/app-MSCN00003"
  cp "$PLAYER/mscharged-np.vpk" "$KIT/mscharged-np-player.vpk"
  mkdir -p "$(dirname "$KIT")/kit-elf" && cp "$PLAYER/mscharged-np.elf" "$(dirname "$KIT")/kit-elf/player.$(sha256sum "$KIT/app-MSCN00003/eboot.bin" | cut -c1-12).elf"
fi
[ -f "$PKG/np-smoke.eboot.bin" ] && cp "$PKG/np-smoke.eboot.bin" "$KIT/eboot-np-smoke.bin"
# O2PKG=<pkg dir of the other code flavour (-O2 without LTO)>: its executable goes in as eboot-np-lab-o2.bin for the A/B step of the session
if [ -n "${O2PKG:-}" ]; then
  cp "$O2PKG/mscharged-np.eboot.bin" "$KIT/eboot-np-lab-o2.bin"
  mkdir -p "$(dirname "$KIT")/kit-elf" && cp "$O2PKG/mscharged-np.elf" "$(dirname "$KIT")/kit-elf/eboot-np-lab-o2.$(sha256sum "$KIT/eboot-np-lab-o2.bin" | cut -c1-12).elf"
fi
cp "$PKG/mscharged-np.map" "$KIT/eboot-np-lab.map" 2>/dev/null || true
# the ELF (13 MB, with symbols) stays on this machine: symbolise a console crash with
#   /opt/vitasdk/bin/arm-vita-eabi-addr2line -f -C -e <elf> <pc>    (the image is not relocated: run-time addresses = link addresses)
mkdir -p "$(dirname "$KIT")/kit-elf" && cp "$PKG/mscharged-np.elf" "$(dirname "$KIT")/kit-elf/eboot-np-lab.$(sha256sum "$KIT/eboot-np-lab.bin" | cut -c1-12).elf"
cp "$HERE"/env/env-np-*.txt "$HERE"/env/np-player-example.txt "$KIT/env/"; cp "$NP/platform/vita/tools/runlog_summary.py" "$KIT/"
cp "$HERE/session_np2.sh" "$HERE/install_promote.sh" "$KIT/"
cp "$HERE/README.md" "$KIT/SESSION-README.md"
mkdir -p "$KIT/psvshell" && cp -r "$HERE"/psvshell/README.md "$HERE"/psvshell/gen_profiles.py "$HERE"/psvshell/profiles "$KIT/psvshell/"   # PIPE: PSVshellPlus 500 MHz profiles (prepared, never installed by the session)
cp "$NP/platform/vita/lab/vitalab.py" "$KIT/"
cp "$NP/platform/vita/lab/README.md" "$KIT/LAB-README.md"
cp "$NP"/tools/nav/*.txt "$KIT/nav-native/" 2>/dev/null || true   # NPB2's scripts for the native layout (onoff values)
cp "$HERE"/nav/*.txt "$KIT/nav/"
cp "$NP/platform/vita/tools/v3k-env/v3k_fulltime.txt" "$KIT/nav/fullmatch.txt"   # the whole-match script that ran in Vita3K
cp /home/claude/msc/tools/scripts/nav_kickoff.txt /home/claude/msc/tools/scripts/nav_match60.txt /home/claude/msc/tools/scripts/nav_menu_idle.txt /home/claude/msc/tools/scripts/mashA.txt "$KIT/nav/"
(cd "$KIT" && sha256sum eboot-np-lab.bin mscharged-np.vpk $(ls eboot-np-smoke.bin eboot-np-lab-o2.bin mscharged-np-player.vpk 2>/dev/null) $(find app-MSCN0000* -type f | sort) > SHA256SUMS)
{
  echo "native port, Vita build (NPV3)"
  echo "np branch when this kit was assembled: $(git -C "$NP" rev-parse --abbrev-ref HEAD) $(git -C "$NP" rev-parse --short HEAD)$(git -C "$NP" diff --quiet || echo '+dirty')"
  echo "built: $(date -u +%F\ %T) UTC; platform layer: $(/opt/vitasdk/bin/arm-vita-eabi-gcc --version | head -1); game objects: $(clang --version | head -1)"
  echo "files:"; (cd "$KIT" && ls -l eboot-np-lab.bin mscharged-np.vpk $(ls eboot-np-smoke.bin eboot-np-lab-o2.bin mscharged-np-player.vpk 2>/dev/null))
  echo "image: $(/opt/vitasdk/bin/arm-vita-eabi-size "$PKG/mscharged-np.elf" | tail -1)"
  echo "the executables were compiled from the np commit named in the lab build id (np-<hash>-<mmdd.hhmmss>) and the main it was based on (git log in that commit)"
  echo "lab build id: $(strings -n 8 "$KIT/eboot-np-lab.bin" | grep -m1 '^np-[0-9a-f]*-[0-9]*\.[0-9]*$' || echo '?')"
  [ -f "$PKG/../ltoinfo.txt" ] && cat "$PKG/../ltoinfo.txt"
  [ -n "${O2PKG:-}" ] && echo "eboot-np-lab-o2.bin image: $(/opt/vitasdk/bin/arm-vita-eabi-size "$O2PKG/mscharged-np.elf" | tail -1)"
} > "$KIT/BUILD-INFO.txt"
cat "$KIT/BUILD-INFO.txt"; echo "kit: $KIT"; du -sh "$KIT"
