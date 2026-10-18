#!/bin/bash
# One Vita3K run of the native package, as one heavy.sh turn (the emulator needs both cores), results kept per tag:
#   platform/vita/tools/job_v3k.sh <tag> <seconds> <env file> [UNTIL pattern] [vpk] [title id]
#   e.g. job_v3k.sh boot1 420 platform/vita/session/env/env-np-frames.txt "reached MSC_MAX_FRAMES"
# NOHEAVY=1 (W5 GLB): the caller already holds a heavy.sh turn (several runs in one turn): the script runs under a plain `timeout` instead.
# Result: build/np/NPV3/v3k-runs/<tag>/{log.txt (the game's), stdout.txt (the emulator's), shots/, end.png, env.txt, run.txt}
cd "$(dirname "$0")/../../.."
TAG=${1:?tag}; SECS=${2:?seconds}; ENV=${3:?env file}; UNTIL=${4:-}
VPK=${5:-/home/claude/msc/build/np/NPV3/vita/pkg/np/mscharged-np.vpk}; TID=${6:-MSCN00001}
export VITASDK=/opt/vitasdk PATH=/opt/vitasdk/bin:$PATH
OUT=${RUNS_DIR:-/home/claude/msc/build/np/NPV3/v3k-runs}/$TAG; rm -rf "$OUT"; mkdir -p "$OUT"   # (RUNS_DIR: VRS, another workstream keeps its runs elsewhere)
P=${PROFILE_DIR:-/home/claude/msc/build/np/NPV3/v3k}
{ echo "tag=$TAG secs=$SECS env=$ENV until=$UNTIL vpk=$VPK ($(sha256sum "$VPK" | cut -c1-16)) start=$(date +%T)"; } > "$OUT/run.txt"
if [ "${NOHEAVY:-0}" = 1 ]; then HV="timeout $((SECS + 90))"; else HV="/home/claude/msc/tools/heavy.sh -t $((SECS + 90))"; fi
GAME=1 ENVTXT=$ENV SHOTEVERY=${SHOTEVERY:-20} FILES="${FILES:-}" \
  $HV platform/vita/tools/v3k_run.sh "$VPK" "$TID" "$SECS" "$UNTIL" >> "$OUT/run.txt" 2>&1
echo "end=$(date +%T)" >> "$OUT/run.txt"
cp "$P/log.txt" "$P/stdout.txt" "$P/end.png" "$OUT/" 2>/dev/null
cp -r "$P/shots" "$OUT/" 2>/dev/null
cp "$P/pref/ux0/data/mscharged/glstats.csv" "$P/pref/ux0/data/mscharged/vgx_frames.csv" "$OUT/" 2>/dev/null   # (W5 GLB: the census csv files of an env file with MSC_VGX_GLSTATS / FRAMESTATS)
cp "$ENV" "$OUT/env.txt"
# the emulator's own log is large: keep only its head and the lines that mention modules, errors and crashes
[ -f "$OUT/stdout.txt" ] && { head -c 300000 "$OUT/stdout.txt" > "$OUT/stdout.head"; grep -a -i "exception\|error\|invalid\|abort\|unimplemented\|not found\|missing\|unresolved" "$OUT/stdout.txt" | head -300 > "$OUT/stdout.errors"; rm -f "$OUT/stdout.txt"; }
[ -f "$OUT/stdout.head" ] && python3 platform/vita/tools/v3k_crash.py "$OUT/stdout.head" "${VPK%.vpk}.elf" > "$OUT/crash.txt" 2>&1 && { echo "--- guest fault (symbolised with the ELF of this build; if the build has changed since the run the names are wrong):"; cat "$OUT/crash.txt"; }
cat "$OUT/run.txt"; ls -la "$OUT" | head -20
