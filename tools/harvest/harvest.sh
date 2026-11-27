#!/bin/bash
# harvest.sh: the emulator shader harvest for Strikers (modelled on Galaxy's tools/seedwalk, 22 Nov 2026).
# Re-records the shader pack of a package until the fast-boot routes show 0 misses.
#
#   tools/harvest/harvest.sh --vpk <package.vpk> --out <work dir outside the repo> [options]
#     --slot 1|2        emulator slot (default 2: /home/claude/work/v3k2, lock /tmp/s13-heavy2.lock; 1 = v3k, /tmp/s13-heavy.lock)
#     --routes "a b"    fast-boot routes (default: fb-vice-goal fb-lava-goal fb-sand fb-pipe fb-crep fb-mega fb-replay fb-tour fb-dump-goal)
#     --base DIR        folder with <key>.pack + vgx_shaderlist.bin to start from (default: the shadercache/ of the package itself)
#     --rounds N        most harvest rounds (default 4); a round = every route once, new gxps merged into the pack
#     --secs S          wall limit of a route run (default 900)
#     --cap N           MSC_WARM_CAP of the HARVEST runs (default 600: warms every marked program, so every key of the list is asked and compiled)
#     --verify-only     no rounds: one verify pass of the package's own pack
# What it does, per round r (r = 1..N):
#   1. harvest pass: the package carries the current pack, each route runs once on a FRESH emulator profile with MSC_WARM_CAP=<cap>; a gxp is written
#      only for a shader the pack does not hold (a compile), so the profile's cache after the run = exactly the pack's misses of that route. The cache and the
#      grown list are copied out inside the lock hold (harvest_one.sh).
#   2. merge: every route's new gxps (a union; one hash = one text) go to the pack with native/tools/seed/assemble_seed.sh (--base: the old pack, nothing is
#      lost), the lists merge with shl.py. No new gxp in a pass = the pack is closed for the routes: go to the verify pass.
# Then the VERIFY pass: every route again on the final package at the SHIPPING warm cap (no env change): the result is OK only when every route ends clean
# with "0 asks it did not answer" and 0 [shaderplay] lines. Output: <out>/final/{874a13e0.pack,vgx_shaderlist.bin}, <out>/final.vpk, <out>/REPORT.txt.
# Needs: the emulator slot's lock (waits), ~3 GB free memory per run, ~1 GB disk. Never ships a compiler or firmware file; the pack holds only shaders the emulator compiled.
set -u
# the script runs from a private copy (bash reads a script while it runs: an edit of the file in the repo must not break a night's run)
if [ -z "${HV_COPY:-}" ]; then
  SRC=$(cd "$(dirname "$0")" && pwd); OUTD=; prev=; for a in "$@"; do [ "$prev" = --out ] && OUTD=$a; prev=$a; done
  [ -n "$OUTD" ] || { sed -n 2,26p "$0"; exit 2; }
  RT=$(cd "$SRC/../.." && pwd); case "$(readlink -m "$OUTD")" in "$RT"/*) echo "harvest.sh: $OUTD is inside the repository: compiled shaders never go there"; exit 2;; esac
  mkdir -p "$OUTD/bin" && cp "$SRC"/harvest.sh "$SRC"/harvest_one.sh "$SRC"/check_gxp_dups.py "$OUTD/bin/" && HV_COPY=1 HV_ROOT=$(cd "$SRC/../.." && pwd) exec bash "$OUTD/bin/harvest.sh" "$@"
fi
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$HV_ROOT
VPK=; OUT=; SLOT=2; ROUTES="fb-vice-goal fb-lava-goal fb-sand fb-pipe fb-crep fb-mega fb-replay fb-tour fb-dump-goal"; BASE=; ROUNDS=4; SECS=900; CAP=600; VONLY=0
while [ $# -gt 0 ]; do case $1 in --vpk) VPK=$2; shift 2;; --out) OUT=$2; shift 2;; --slot) SLOT=$2; shift 2;; --routes) ROUTES=$2; shift 2;;
  --base) BASE=$2; shift 2;; --rounds) ROUNDS=$2; shift 2;; --secs) SECS=$2; shift 2;; --cap) CAP=$2; shift 2;; --verify-only) VONLY=1; shift;; *) sed -n 2,26p "$0"; exit 2;; esac; done
[ -f "$VPK" ] && [ -n "$OUT" ] || { sed -n 2,26p "$0"; exit 2; }
VPK=$(readlink -f "$VPK"); OUT=$(readlink -m "$OUT")
case "$OUT" in "$ROOT"/*) echo "harvest.sh: $OUT is inside the repository: compiled shaders never go there"; exit 2;; esac
FB=$ROOT/tools/console-1.2/fastboot
mkdir -p "$OUT"
log() { echo "$(date +%T) $*" | tee -a "$OUT/progress.txt"; }

# the route's env file: the fast-boot env of the route (sand has none of its own: the dump env), the route named in it, MSC_WARM_CAP set (capped = none)
mkenv() { # route cap outfile
  local r=$1 cap=$2 o=$3 e=$FB/env/$1.txt
  [ -f "$e" ] || e=$FB/env/fb-dump-goal.txt
  sed "s#fb-[a-z0-9-]*\.txt#$r.txt#" "$e" | grep -v '^MSC_WARM_CAP=' > "$o"; echo >> "$o"
  [ -z "$cap" ] || echo "MSC_WARM_CAP=$cap" >> "$o"
  # a shader-TEXT switch in a route's env would record a pack for texts the shipping plan does not make (Galaxy's seedwalk refuses them for the same reason): refuse
  if grep -E "^(MSC_VGX_(ATRIVIAL|BLENDNODISCARD|DISCARD_BLEND|FINCLAMP|FOGVTX|FS_HALF|FS_PROJ|FS_TRIM|GENSTAT|HSR_FACTS|HSR_PROVEN|LEANVARY|PALIDX|TEV_CANON|TEX2D|UNIPACK|VS_NOBRANCH|VS_TRIM|ZFREEZE|VSOUT|TEXVEC2)|MSC_VGX_SHADERPACK|MSC_VGX_SEED)=" "$o" > "$OUT/refused-env.txt"; then
    log "refused: $o sets a shader-text switch ($(tr '\n' ' ' < "$OUT/refused-env.txt")): the recorded pack would not be the shipping one (the plan's defaults must decide the texts)"; exit 2
  fi
}
# a package with <pack dir> instead of its shadercache/
mkvpk() { bash "$ROOT/native/tools/seed/pack_vpk.sh" "$VPK" "$1" "$2" > /dev/null || { log "pack_vpk failed"; exit 1; }; }

CUR=$OUT/pack-r0
if [ ! -f "$CUR/vgx_shaderlist.bin" ]; then
  mkdir -p "$CUR"
  if [ -n "$BASE" ]; then cp -a "$BASE"/. "$CUR"/; else unzip -q -o "$VPK" 'shadercache/*' -d "$OUT/unz" && cp -a "$OUT/unz/shadercache"/. "$CUR"/ && rm -rf "$OUT/unz"; fi
fi
ls "$CUR"/*.pack > /dev/null 2>&1 || { log "no .pack in $CUR"; exit 1; }
KEY=$(basename "$(ls "$CUR"/*.pack | head -1)" .pack)
log "start: vpk $VPK, base pack $(sha256sum "$CUR/$KEY.pack" | cut -c1-12), key $KEY, routes: $ROUTES, slot $SLOT, harvest cap $CAP"

pass() { # name pack-dir cap incremental(0|1) -> $OUT/<name>/<route>/ ; sets PASS_NEW (new gxp count over the routes) and PASS_PACK (the pack dir after the pass)
  local name=$1 pk=$2 cap=$3 incr=${4:-0} r v=$OUT/$1.vpk
  mkvpk "$pk" "$v"; PASS_NEW=0
  for r in $ROUTES; do
    mkenv "$r" "$cap" "$OUT/$name-$r.env"
    log "$name: $r start"
    export STOPON='in-process app relaunch|Game closed|The app exited'   # (no stop on the emulator's own |C| lines: Vita3K cannot translate some gxps, "Error in get/compiled vertex shader", and skips that draw; the count is in the result: emu_critical)
    bash "$HERE/harvest_one.sh" "$SLOT" "hv-$name-$r" "$r" "$OUT/$name-$r.env" "$v" "$OUT/$name/$r" "$SECS" || { log "harvest_one refused (rc $?): no network namespace? (HV_NETNS=0 runs without isolation)"; exit 1; }
    local res=$OUT/$name/$r/result.txt n u
    n=$(grep -o 'gxp_new=[0-9]*' "$res" | cut -d= -f2); u=$(grep '^unanswered=' "$res" | cut -d= -f2)
    PASS_NEW=$((PASS_NEW + ${n:-0}))
    log "$name: $r: $(sed -n 1,2p "$res" | tr '\n' ' ' | cut -c1-300) unanswered=${u:-?}"
    # the same gxp name from two routes (or two rounds) must be byte-identical: checked after EVERY route against every cache so far (the merge below sees one route at a time in an incremental pass)
    if ! python3 -I "$HERE/check_gxp_dups.py" "$OUT"/*/*/cache > "$OUT/gxp-dups.txt" 2>&1; then
      log "gxp CONFLICT: same name, different bytes across routes (see gxp-dups.txt): the harvest stops"; cat "$OUT/gxp-dups.txt"; exit 1
    fi
    if [ "$incr" = 1 ] && [ "${n:-0}" -gt 0 ]; then   # the next route starts from a pack that has this one's compiles: its run compiles only what is still missing (minutes saved)
      merge_round "$name-$r" "$pk" "$OUT/pack-$name-$r" "$r"; pk=$OUT/pack-$name-$r; mkvpk "$pk" "$v"
    fi
  done
  PASS_PACK=$pk
}
merge_round() { # tag pack-in pack-out routes...  (the lists and gxps the routes' runs of the pass left)
  local tag=$1 pin=$2 pout=$3 pref=$OUT/pref-$1 lists=() r; shift 3
  rm -rf "$pref"; mkdir -p "$pref/ux0/data/mscharged-perf-test/shadercache/$KEY/v3/v" "$pref/ux0/data/mscharged-perf-test/shadercache/$KEY/v3/f"
  for r in "$@"; do
    local c=$OUT/${tag%%-fb-*}/$r/cache
    # one hash = one text = one gxp: the same file name with other bytes from another route means the compiler is not deterministic (or a text switch differed): stop, never pick one
    if [ -d "$c/$KEY/v3" ]; then
      ( cd "$c/$KEY/v3" && find . -name '*.gxp' ) | while read -r f; do
        d="$pref/ux0/data/mscharged-perf-test/shadercache/$KEY/v3/$f"
        if [ -f "$d" ] && ! cmp -s "$d" "$c/$KEY/v3/$f"; then echo "CONFLICT $f ($r)" >> "$OUT/conflicts.txt"; fi
      done
      [ -s "$OUT/conflicts.txt" ] && { log "gxp CONFLICT: same name, different bytes (see conflicts.txt): the harvest stops"; exit 1; }
    fi
    [ -d "$c/$KEY/v3" ] && cp -a "$c/$KEY/v3/." "$pref/ux0/data/mscharged-perf-test/shadercache/$KEY/v3/"
    [ -f "$c/vgx_shaderlist.bin" ] && lists+=("$c/vgx_shaderlist.bin")
  done
  python3 -I "$ROOT/native/tools/seed/shl.py" merge -o "$OUT/list-$tag.bin" "$pin/vgx_shaderlist.bin" "${lists[@]}" > "$OUT/merge-$tag.txt" 2>&1 || { log "shl merge failed"; cat "$OUT/merge-$tag.txt"; exit 1; }
  rm -rf "$pout"
  bash "$ROOT/native/tools/seed/assemble_seed.sh" "$pref" "$OUT/list-$tag.bin" "$pout" --base "$pin" "$KEY" > "$OUT/assemble-$tag.txt" 2>&1 || { log "assemble failed"; cat "$OUT/assemble-$tag.txt"; exit 1; }
  log "$tag: $(head -1 "$OUT/assemble-$tag.txt"); pack $(sha256sum "$pout/$KEY.pack" | cut -c1-12); list $(python3 -I "$ROOT/native/tools/seed/shl.py" info "$pout/vgx_shaderlist.bin" 2>&1 | tail -1 | cut -c1-110)"
  rm -rf "$pref"
}

if [ $VONLY = 0 ]; then
  for i in $(seq 1 "$ROUNDS"); do
    pass "h$i" "$CUR" "$CAP" 1
    CUR=$PASS_PACK
    log "round $i: $PASS_NEW new gxp files over all routes; pack now $(sha256sum "$CUR/$KEY.pack" | cut -c1-12)"
    if [ "$PASS_NEW" = 0 ]; then log "round $i: nothing new: the pack is closed for these routes"; break; fi
  done
fi
rm -rf "$OUT/final"; cp -a "$CUR" "$OUT/final"; mkvpk "$OUT/final" "$OUT/final.vpk"
pass verify "$OUT/final" ""
{
  echo "HARVEST REPORT $(date '+%F %T')  vpk $VPK"
  echo "final pack: $OUT/final/$KEY.pack sha256 $(sha256sum "$OUT/final/$KEY.pack" | cut -c1-16), list $OUT/final/vgx_shaderlist.bin; test package $OUT/final.vpk"
  ok=1
  for r in $ROUTES; do
    res=$OUT/verify/$r/result.txt; u=$(grep '^unanswered=' "$res" | cut -d= -f2); sp=$(grep -o 'shaderplay=[0-9]*' "$res" | cut -d= -f2); fl=$(grep -o 'faults=[0-9]*' "$res" | cut -d= -f2)
    cm=$(grep -o 'complete=[0-9]*' "$res" | cut -d= -f2)
    st=OK; { [ "${u:-x}" = 0 ] && [ "${sp:-x}" = 0 ]; } || { st="MISS"; ok=0; }
    [ "${cm:-0}" = 1 ] || { st="$st+ROUTE-NOT-COMPLETE"; ok=0; }
    [ "${fl:-0}" = 0 ] || st="$st+FAULT($fl)"
    ec=$(grep -o 'emu_critical=[0-9]*' "$res" | cut -d= -f2); [ "${ec:-0}" = 0 ] || st="$st+EMU-CRITICAL($ec: Vita3K could not translate a shader; see the log)"
    echo "  $r: $st unanswered=${u:-?} shaderplay=${sp:-?} | $(sed -n 1p "$res" | cut -c1-200)"
  done
  [ $ok = 1 ] && echo "RESULT: 0 misses on every route" || echo "RESULT: misses remain (see lines above)"
} | tee "$OUT/REPORT.txt"
