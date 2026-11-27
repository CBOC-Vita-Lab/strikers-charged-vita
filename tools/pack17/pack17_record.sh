#!/bin/bash
# pack17_record.sh <worktree> <host32 glnull exe> <route.txt> <name> <out dir> [KEY=VAL ...]
# One host run of a route that records the shader keys the game asks for (MSC_VGX_SHADERLIST) into <out dir>/<name>.shl, with the Vita plan's
# defaults in the environment (kSpeed + kQuality of vita_plan.cpp, then native/tools/seed/env_player.sh for the rest that changes keys), as the
# shader seed procedure does (docs/BUILDING.md, "The keys the new renderer makes"). The key struct is the same on host and console, so the list says
# which texts a console would ask for. Needs the disc (NP_DISC_DIR) and a glnull host32 build: nothing is compiled by a shader compiler.
#   NP_DISC_DIR=/path/to/extracted/disc tools/pack17/pack17_record.sh . build/host32/np-host32-full-glnull tools/console-1.2/fastboot/routes/fb-dump.txt dump out/
set -u
WT=$(cd "${1:?worktree}" && pwd); EXE=$(realpath "${2:?exe}"); ROUTE=$(realpath "${3:?route}"); NAME=${4:?name}; OUT=${5:?out dir}; shift 5
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
: "${NP_DISC_DIR:?set NP_DISC_DIR to the extracted disc}"
W=$(mktemp -d "${TMPDIR:-/tmp}/p17rec.XXXXXX"); trap 'rm -rf "$W"' EXIT
mkdir -p "$W/data"
PLAN=$(python3 -I "$WT/tools/pack17/plan_env.py" "$WT")
rm -f "$OUT/$NAME.shl"
(
  cd "$W/data" || exit 2
  for kv in $PLAN; do export "$kv"; done
  . "$WT/native/tools/seed/env_player.sh"
  for kv in "$@"; do export "$kv"; done
  export NP_DISC_DIR NP_DATA_DIR="$W/data" MSC_INPUT_SCRIPT="$ROUTE" MSC_VIRTUAL_TIME=1 MSC_MAX_FRAMES=${MAXF:-9000} MSC_FRAME_LOG=100000 MSC_AI_BUDGET=off NP_TEST_NETSEED=777 \
         MSC_VGX_SHADERLIST="$OUT/$NAME.shl"
  timeout -k 5 "${TMO:-900}" "$EXE" > "$OUT/$NAME.log" 2>&1
  echo "rc=$?" >> "$OUT/$NAME.log"
)
echo "pack17_record $NAME: $(tail -1 "$OUT/$NAME.log"), list $(stat -c %s "$OUT/$NAME.shl" 2>/dev/null || echo none) bytes"
