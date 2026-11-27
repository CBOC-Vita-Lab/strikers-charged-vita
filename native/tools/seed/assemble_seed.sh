#!/bin/bash
# Steps 5 of docs/BUILDING.md "Making The Shader Seed": the pack of the shaders a Vita3K profile compiled (plus the earlier pack) and the warm-up list.
#   tools/seed/assemble_seed.sh <profile pref dir> <warm-up list.shl> <out pack dir> [--base <dir with <key>.pack files> [--base-key <older key>]] [key]
# <profile pref dir>: .../v3k/pref (its ux0/data/mscharged/shadercache/<key>/v3 holds the GXPs vitaGL wrote)
# <out pack dir>: gets <key>.pack and vgx_shaderlist.bin: what tools/repack-release.py --shadercache and pack_vpk.sh take.
# The last argument may name the cache key (default: the only key folder the profile holds).
set -e
PREF=${1:?profile pref dir}; LIST=${2:?warm-up list}; OUT=${3:?out pack dir}; shift 3
BASE=(); KEY=
while [ $# -gt 0 ]; do case $1 in --base) BASE+=(--base "$2"); shift 2;; --base-key) BASE+=(--base-key "$2"); shift 2;; *) KEY=$1; shift;; esac; done
HERE=$(cd "$(dirname "$0")/../.." && pwd)
C=$PREF/ux0/data/${SEED_DATA:-mscharged-perf-test}/shadercache   # (1.3 workspace: the isolated player build CDXMP0001; SEED_DATA=mscharged for a lab build MSCN00001)
[ -n "$KEY" ] || KEY=$(cd "$C" && ls -d */ | tr -d / | head -1)
[ -d "$C/$KEY" ] || { echo "no $C/$KEY (the profile has no shader cache folder: did the emulator run?)"; exit 1; }
T=$(mktemp -d "${TMPDIR:-/tmp}/seedXXXX")
mkdir -p "$T/$KEY/v3/v" "$T/$KEY/v3/f"; [ ! -d "$C/$KEY/v3" ] || cp -a "$C/$KEY/v3/." "$T/$KEY/v3/"   # (nothing compiled: the pack is the base pack again)
cp "$LIST" "$T/vgx_shaderlist.bin"
echo "new shaders in the profile: $(find "$T/$KEY/v3" -name '*.gxp' | wc -l) files"
python3 -I "$HERE/platform/vita/tools/make_shader_pack.py" "${BASE[@]}" "$T" "$OUT"
rm -rf "$T"
ls -l "$OUT"
