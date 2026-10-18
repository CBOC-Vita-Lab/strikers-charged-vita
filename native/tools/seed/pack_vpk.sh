#!/bin/bash
# W5-SEED: a copy of a package with a shader seed added (a .vpk is a zip: nothing is rebuilt)
#   tools/seed/pack_vpk.sh <in.vpk> <seed dir (with <key>/v2/... and vgx_shaderlist.bin)> <out.vpk>
# The package gets shadercache/... exactly as Makefile.vpk's NP_SHADER_SEED would pack it. An existing shadercache/ in <in.vpk> is replaced.
set -e
IN=$(realpath "${1:?in.vpk}"); SEED=$(realpath "${2:?seed dir}"); OUT=$(realpath -m "${3:?out.vpk}")
T=$(mktemp -d /home/claude/msc/build/np/W5SEED/packXXXX)
cp "$IN" "$OUT"
zip -q -d "$OUT" 'shadercache/*' 2>/dev/null || true
mkdir -p "$T/shadercache"; cp -a "$SEED"/. "$T/shadercache/"
( cd "$T" && zip -q -r -X -D "$OUT" shadercache )
rm -rf "$T"
echo "$OUT: $(stat -c %s "$OUT") bytes ($(stat -c %s "$IN") before), $(unzip -l "$OUT" | grep -c 'shadercache/.*gxp') gxp"
