#!/bin/bash
# pack17_build.sh: compile the shader texts of a pack17 request (tools/pack17/pack17_request.py) on YOUR machine and merge them into the old pack.
#
# Why here: the shader compiler is Sony's libshacccg (Vita3K runs it). It is not in this repository, is not shipped and is never copied by this script:
# it must already be in your Vita3K profile (the way Vita3K itself asks you to install it). The script only drives Vita3K and the repository's own seed tools
# (native/tools/seed/): the same steps as docs/BUILDING.md "Top-up", steps 4 and 5, for one list. It refuses to finish if a libshacccg file appears in the output.
#
# usage: tools/pack17/pack17_build.sh -b <base pack folder> -r <pack17-request.shl> -l <lab package .vpk> -o <out folder> [-H <hashlist exe>] [-n]
#   -b  the shipped pack's folder: <key>.pack and vgx_shaderlist.bin (shadercache/ of the release package, unzipped; pack16 = 874a13e0.pack)
#   -r  pack17-request.shl, from pack17_request.py
#   -l  a lab package built from the same tree as the build the pack is for (the isolated player build CDXMP0001 of b13x.sh/stage-hardware-test.py works): the game
#       itself warms the list in Vita3K and compiles what the pack lacks
#   -o  output: <out>/pack17/<key>.pack + vgx_shaderlist.bin (the OLD warm-up list: the request is for pack misses, not for start-up) and <out>/report.txt
#   -H  tools/pack17/hashlist (build_hashlist.sh); default <out>/hashlist built here
#   -n  dry run: check the inputs and print the steps, start nothing
# Environment: V3K=<folder of Vita3K's run.sh> (default /home/claude/work/v3k), SEED_ROOT=<work folder> (default <out>/seed), SEED_DATA (default mscharged-perf-test).
# After it: python3 tools/repack-release.py ... --shadercache <out>/pack17 ... (BUILDING.md step 6), then update the hashes in cboc-release.json, RELEASE.json, SHA256SUMS.
set -euo pipefail
BASE=; REQ=; LAB=; OUT=; HL=; DRY=0
while getopts "b:r:l:o:H:n" o; do case $o in b) BASE=$OPTARG;; r) REQ=$OPTARG;; l) LAB=$OPTARG;; o) OUT=$OPTARG;; H) HL=$OPTARG;; n) DRY=1;; *) exit 2;; esac; done
[ -n "$BASE" ] && [ -n "$REQ" ] && [ -n "$LAB" ] && [ -n "$OUT" ] || { sed -n 2,22p "$0"; exit 2; }
R=$(cd "$(dirname "$0")/../.." && pwd)
KEYPACK=$(ls "$BASE"/*.pack 2>/dev/null | head -1); [ -n "$KEYPACK" ] || { echo "no <key>.pack in $BASE"; exit 1; }
[ -f "$BASE/vgx_shaderlist.bin" ] || { echo "no vgx_shaderlist.bin in $BASE"; exit 1; }
[ -f "$REQ" ] || { echo "no request file $REQ (pack17_request.py writes none when the pack answers everything: then there is nothing to do)"; exit 1; }
[ -f "$LAB" ] || { echo "no lab package $LAB"; exit 1; }
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
export SEED_ROOT=${SEED_ROOT:-$OUT/seed}
echo "base pack $KEYPACK ($(stat -c %s "$KEYPACK") bytes); request $(python3 -I "$R/native/tools/seed/shl.py" info "$REQ" | tr '\n' ' ')"
if [ "$DRY" = 1 ]; then
  echo "dry run. Steps: 1 hashlist build; 2 pack_vpk.sh lab+base pack; 3 v3k_warm.sh (Vita3K compiles the request with YOUR libshacccg); 4 assemble_seed.sh --base; 5 pack17_check.py; 6 no-libshacccg check"
  exit 0
fi
[ -n "$HL" ] || { HL=$OUT/hashlist; bash "$R/tools/pack17/build_hashlist.sh" "$R" "$HL"; }
echo "== 2: the old pack into the lab package"
mkdir -p "$OUT/base"; cp "$BASE"/*.pack "$BASE/vgx_shaderlist.bin" "$OUT/base/"
bash "$R/native/tools/seed/pack_vpk.sh" "$LAB" "$OUT/base" "$OUT/lab-base.vpk"
echo "== 3: Vita3K compiles what the pack lacks (your libshacccg; the log's 'frame 1: N shader compiles' is the count)"
rm -rf "$SEED_ROOT/v3kp"
bash "$R/native/tools/seed/v3k_warm.sh" -n 60 -s "${WARM_SECS:-900}" -v "$OUT/lab-base.vpk" pack17 "$REQ" || { echo "the warm-up run failed: see $SEED_ROOT/v3k-runs/pack17/log.txt (a missing shader compiler shows as 0 compiles and 0 new .gxp files)"; exit 1; }
PREF=$SEED_ROOT/v3kp/pref
NG=$(find "$PREF/ux0/data/${SEED_DATA:-mscharged-perf-test}/shadercache" -name '*.gxp' 2>/dev/null | wc -l)
echo "compiled shaders (.gxp) in the profile: $NG"
[ "$NG" -gt 0 ] || { echo "nothing was compiled: is libshacccg installed in the Vita3K profile?"; exit 1; }
echo "== 4: assemble (the old pack's shaders are kept, the warm-up list stays the old one)"
bash "$R/native/tools/seed/assemble_seed.sh" "$PREF" "$BASE/vgx_shaderlist.bin" "$OUT/pack17" --base "$OUT/base"
echo "== 5: does the new pack answer the request, and keep everything the old one had"
python3 -I "$R/tools/pack17/pack17_check.py" "$OUT/pack17/$(basename "$KEYPACK")" "$REQ" "$HL" --base-pack "$KEYPACK" | tee "$OUT/report.txt"
echo "== 6: no shader compiler in the output"
if find "$OUT/pack17" "$OUT/lab-base.vpk" -iname '*libshacccg*' 2>/dev/null | grep -q .; then echo "libshacccg found in the output: remove it, do not ship it"; exit 1; fi
if unzip -l "$OUT/lab-base.vpk" | grep -qi libshacccg; then echo "libshacccg inside lab-base.vpk: do not ship that file"; exit 1; fi
echo "pack17 ready: $OUT/pack17/$(basename "$KEYPACK") ($(stat -c %s "$OUT/pack17/$(basename "$KEYPACK")") bytes), sha256 $(sha256sum "$OUT/pack17/$(basename "$KEYPACK")" | cut -c1-16)"
