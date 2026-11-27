#!/bin/bash
# W5-SEED: assembles the package seed directory from a Vita3K profile that compiled the whole catalogue.
#   tools/seed/make_seed.sh <profile pref dir> <list.shl> <out dir> [key=bc447993]
# <profile pref dir>: .../v3k/pref  (its ux0/data/mscharged/shadercache/<key>/v3 holds the GXPs vitaGL wrote; v2 for a session of a vitaGL before 9a14d17)
# Result: <out dir>/<key>/<v3 or v2>/{v,f}/HH/*.gxp  and  <out dir>/vgx_shaderlist.bin (= the list), exactly what NP_SHADER_SEED expects.
# The profile must be one that compiled with a package WITHOUT a seed (so its cache holds the catalogue's shaders and the back end's copy pair and the host's
# self-test pair, nothing stale). The check at the end (seedcheck: every key of the list -> the file name vitaGL gives it) must say 0 missing.
set -e
PREF=${1:?profile pref dir}; LIST=${2:?list}; OUT=${3:?out dir}; KEY=${4:-bc447993}
HERE=$(cd "$(dirname "$0")/../.." && pwd)
SC=${SEEDCHECK:-${SEED_ROOT:-/work/msc/build/np/W5SEED}/tools/seedcheck}
VER=v3; [ -d "$PREF/ux0/data/mscharged/shadercache/$KEY/v3" ] || VER=v2
SRC=$PREF/ux0/data/mscharged/shadercache/$KEY/$VER
[ -d "$SRC" ] || { echo "no $SRC"; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT/$KEY"
cp -a "$SRC" "$OUT/$KEY/$VER"
cp "$LIST" "$OUT/vgx_shaderlist.bin"
echo "cache: $(find $OUT/$KEY/$VER -name '*.gxp' | wc -l) files, $(du -sk $OUT | cut -f1) KiB"
$SC "$LIST" "$OUT/$KEY/$VER" | tail -3
if ! $SC "$LIST" "$OUT/$KEY/$VER" | tail -1 | grep -q ' 0 missing'; then echo "SEED INCOMPLETE"; exit 1; fi
extra=$(( $(find $OUT/$KEY/$VER -name '*.gxp' | wc -l) - $($SC "$LIST" --names | grep -c gxp) ))
echo "seed ok: $(find $OUT/$KEY/$VER -name '*.gxp' | wc -l) files ($extra not named by the list: the copy pair and the self-test pair), $(du -sk $OUT | cut -f1) KiB, list $(stat -c %s $OUT/vgx_shaderlist.bin) bytes"
