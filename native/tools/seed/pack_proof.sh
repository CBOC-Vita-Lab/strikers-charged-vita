#!/bin/bash
# The pack proof of docs/BUILDING.md step 6 with the headless emulator's run.sh (no job_v3k.sh): a cold install of a package, one route, then cache.txt.
#   pack_proof.sh <tag> <vpk> <route.txt> [seconds] [VAR=val ...]      (holds the emulator's lock; V3K=<dir of run.sh>, default /home/claude/work/v3k)
# V3K_PREF / V3K_XDG / V3K_RUNS (see v3k_emu.sh): a private emulator profile, so the cold install does not clear other users' data root; the run goes through v3k_emu.sh then.
# Fresh data root, so every .gxp in the cache folder at the end was compiled by the console's shader compiler in this run.
# cache.txt: those files, the log's compile count, GUEST FAULTS, the C heap's peak. PASS needs no file, no fault, and the route's end in the log.
V=${V3K:-/home/claude/work/v3k}; TAG=${1:?tag}; VPK=$(realpath "${2:?vpk}"); ROUTE=$(realpath "${3:?route}"); SECS=${4:-3000}; shift 3; [ $# -gt 0 ] && shift
EMU=$(realpath "$(dirname "$0")")/v3k_emu.sh; PREFD=${V3K_PREF:-$V/pref}; RUNS=${V3K_RUNS:-$V/runs}; D=$PREFD/ux0/data/mscharged-perf-test; T=$(mktemp -d); E=$T/env.txt; B=$(basename "$ROUTE")
{ echo "MSC_INPUT_SCRIPT=ux0:data/mscharged-perf-test/$B"; echo MSC_FRAME_TIMING=300; echo MSC_WATCH=1; echo MSC_WATCH_EVERY=600; echo MSC_MAX_FRAMES=200000
  case " $* " in *" MSC_VITA_RENDER="*) ;; *) echo MSC_VITA_RENDER=320x224;; esac
  for kv in "$@"; do [ "$kv" = "MSC_VITA_RENDER=" ] || echo "$kv"; done; } > "$E"
cd "$V" || exit 2
TAG=$TAG FILES=$ROUTE SHOTEVERY=${SHOTEVERY:-60} UNTIL='script exit|\[script\] f=[0-9]+ exit|reached MSC_MAX_FRAMES|FATAL' flock emu.lock bash -c '
  $4 "$0" "$1" "$2"; R=$5/$TAG; L=$R/log.txt; G=$(find "$3/shadercache" -name "*.gxp" 2>/dev/null | wc -l)
  F=$(grep -a -c "invalid_access" $R/stdout.txt); C=$(grep -a "shader work so far" $L | tail -1 | sed "s/.*so far: //")
  P=$(grep -a -o -E "C heap [0-9]+ KiB in use, peak [0-9]+" $L | sed "s/.*peak //" | sort -n | tail -1)
  U=$(grep -a -o -E "C heap in use [0-9]+ KiB" $L | sed "s/C heap in use //; s/ KiB//" | sort -n | tail -1)
  E=$(grep -a -c -E "script exit|\[script\] f=[0-9]+ exit" $L); Q=$(grep -a -E "\[vgx\] MSC_VGX_WARM_HEAP|FATAL|\[fatal\]|abort" $L | head -3)
  { echo "package: $0 ($(stat -c %s "$0") bytes)"
    echo "compiled by the shader compiler in this run (gxp files in the cache folder): $G"
    echo "log: $C"
    echo "GUEST FAULTS: $F"
    echo "C heap peak: ${P:-?} KiB (highest 30 s line: ${U:-?} KiB) of 65536"
    echo "route ended by its own exit: $E"
    [ -z "$Q" ] || echo "notes: $Q"
    [ "$G" = 0 ] && [ "$F" = 0 ] && [ "$E" -ge 1 ] && echo PASS || echo FAIL; } | tee $R/cache.txt
  find "$3/shadercache" -name "*.gxp" 2>/dev/null > $R/compiled.txt
' "$VPK" "$E" "$SECS" "$D" "$EMU" "$RUNS"
rm -rf "$T"
