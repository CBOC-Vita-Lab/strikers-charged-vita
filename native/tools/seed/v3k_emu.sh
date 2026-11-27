#!/bin/bash
# (copy of /home/claude/work/v3k/run.sh for the shader-pack tools: V3K_PREF=<dir> and V3K_XDG=<dir> name a PRIVATE profile and emulator config (pref-path in
#  <xdg>/config/Vita3K/config.yml must point at the pref dir), V3K_RUNS=<dir> where the run folders go; so a seed's cache is not touched by other users of the emulator.
#  Callers hold /home/claude/work/v3k/emu.lock (flock) themselves.)
# One headless Vita3K run of a package: install, launch under a private Xvfb (OpenGL on llvmpipe), keep the game's log and a screenshot, stop.
#   /home/claude/work/v3k/run.sh <vpk> <env file> <seconds>
# The env file goes into the title's data root as BOTH env.txt and np-player.txt (lab builds read the first, player builds the second).
# Data root: ux0:data/mscharged-perf-test for the isolated test app CDXMP0001, else ux0:data/mscharged. Paths in env files are the title's own.
# 2026-11-06: emulator is now built from source (Vita3K master 2f68c1b, Qt 6.11.0 in /opt/qt, clang-20, libstdc++ 14; build/bin/Vita3K symlinks the build output).
#   LIBGL_DRIVERS_PATH is pinned to the amd64 system Mesa DRI dir (i386 Mesa was installed alongside; never let GL pick the i386 dir). Config backend stays OpenGL.
# Optional environment:
#   TAG=<name>       result folder name (default: time stamp)
#   UNTIL=<egrep>    end the run when the game's log matches (default below; not STALL lines, the long first frame in software GL makes them); empty = always run <seconds>
#   STOPON=<egrep>   end the run when the emulator's output matches (default: a critical line or the app closing)
#   SHOTEVERY=<s>    a screenshot every s seconds into shots/ (default 15)
#   GAME_DIR=<dir>   the extracted disc, linked as ux0:data/mscharged/game (default /home/claude/work/disc/extract-strikers; GAME_DIR= for none:
#                    the game then stops at its missing-data message). The disc is never written; a run reports any file in it that changed.
#   FILES="a b"      extra files copied into the data root (nav scripts: tools/console-1.2/routes/tour.txt ...)
#   LOGLEVEL=<0-6>   the emulator's log level (default 2, info; 0 is trace and floods)
#   KEEPDATA=1       keep the data root and the emulator's shader cache from the last run (saves, compiled shaders: far quicker); log and env are still replaced
# Result: /home/claude/work/v3k/runs/<tag>/{log.txt, stdout.txt, stdout.errors, vita3k.log, end.png, shots/ (X screen), gameshots/ (the game's own), env.txt, run.txt}
# Exit: 0 when the game wrote a log, 1 otherwise, 2 on bad usage. Everything the script started is gone when it returns.
set -u
V=/home/claude/work/v3k
BIN=$V/build/bin/Vita3K
VPK=$(realpath "${1:?usage: run.sh <vpk> <env file> <seconds>}") || exit 2
ENVF=$(realpath "${2:?env file}") || exit 2
SECS=${3:?seconds}
[ -x "$BIN" ] || { echo "no emulator at $BIN"; exit 2; }
[ -f "$VPK" ] && [ -f "$ENVF" ] || { echo "vpk or env file not found"; exit 2; }
[ -n "${UNTIL+x}" ] || UNTIL='the game data is missing|reached MSC_MAX_FRAMES|script exit|FATAL|CRASH'
[ -n "${STOPON+x}" ] || STOPON='\|C\||in-process app relaunch|Game closed|The app exited'
SHOTEVERY=${SHOTEVERY:-15}
TAG=${TAG:-$(date +%Y%m%d-%H%M%S)}
OUT=${V3K_RUNS:-$V/runs}/$TAG; rm -rf "$OUT"; mkdir -p "$OUT/shots"
PREF=${V3K_PREF:-$V/pref}; XDG=${V3K_XDG:-$V/xdg}
export XDG_CONFIG_HOME=$XDG/config XDG_CACHE_HOME=$XDG/cache XDG_DATA_HOME=$XDG/data
CFG=$XDG_CONFIG_HOME/Vita3K/config.yml
mkdir -p "$(dirname "$CFG")" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
[ -f "$CFG" ] || printf 'pref-path: %s/\nbackend-renderer: OpenGL\n' "$PREF" > "$CFG"

# the title id comes from the package's param.sfo
TID=$(python3 -I - "$VPK" <<'E'
import struct, sys, zipfile
s = zipfile.ZipFile(sys.argv[1]).read('sce_sys/param.sfo')
key, data, n = struct.unpack('<4x4xIII', s[:20])
for i in range(n):
    k, fmt, used, cap, off = struct.unpack('<HHIII', s[20 + i * 16:36 + i * 16])
    if s[key + k:s.index(b'\0', key + k)] == b'TITLE_ID':
        print(s[data + off:data + off + used].rstrip(b'\0').decode())
E
)
[ -n "$TID" ] || { echo "no TITLE_ID in $VPK"; exit 2; }
echo "tag=$TAG tid=$TID secs=$SECS vpk=$VPK ($(sha256sum "$VPK" | cut -c1-16)) env=$ENVF start=$(date +%T)" | tee "$OUT/run.txt"

# install (as the project's v3k_run.sh does: the package's files under ux0:app/<tid>), with an empty taiHEN config: no kubridge in the emulator
case $TID in CDXMP0001) D=$PREF/ux0/data/mscharged-perf-test;; *) D=$PREF/ux0/data/mscharged;; esac
GAME_DIR=${GAME_DIR-/home/claude/work/disc/extract-strikers}
rm -rf "$PREF/ux0/app/$TID"
[ "${KEEPDATA:-0}" = 1 ] || rm -rf "$XDG_CACHE_HOME/Vita3K/shaders/$TID" "$XDG_CACHE_HOME/Vita3K/shaderlog"
mkdir -p "$PREF/ux0/app/$TID" "$D" "$PREF/ux0/data/mscharged" "$PREF/ur0/tai"
: > "$PREF/ur0/tai/config.txt"
unzip -q -o "$VPK" -d "$PREF/ux0/app/$TID" || { echo "unzip failed"; exit 1; }
[ "${KEEPDATA:-0}" = 1 ] || find "$D" -mindepth 1 -maxdepth 1 ! -name game ! -name nand -exec rm -rf {} + 2>/dev/null
rm -f "$PREF/ux0/data/mscharged/game" "$D/log.txt" "$D/lab.txt" "$D/np-plan.txt"   # (np-plan.txt: a slow first frame in here must not pick the next run's fallback plan)
[ -n "$GAME_DIR" ] && ln -sfn "$GAME_DIR" "$PREF/ux0/data/mscharged/game"
touch "$OUT/.start"
for f in ${FILES:-}; do cp "$f" "$D/"; done
cp "$ENVF" "$D/env.txt"; cp "$ENVF" "$D/np-player.txt"; cp "$ENVF" "$OUT/env.txt"
: > "$D/nolab"   # lab mode would wait for a client that never comes

# a private X display
N=90; while [ -e "/tmp/.X$N-lock" ] || [ -e "/tmp/.X11-unix/X$N" ]; do N=$((N + 1)); done
Xvfb ":$N" -screen 0 1280x800x24 -nolisten tcp +extension GLX >"$OUT/xvfb.txt" 2>&1 & XPID=$!
PID=
stopall() {
  [ -n "$PID" ] && { kill "$PID" 2>/dev/null; for _ in 1 2 3 4 5 6; do kill -0 "$PID" 2>/dev/null || break; sleep 0.5; done; kill -9 "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; }
  PID=
  [ -n "$XPID" ] && { kill "$XPID" 2>/dev/null; wait "$XPID" 2>/dev/null; }
  XPID=
}
trap stopall EXIT; trap 'exit 130' INT TERM
for _ in $(seq 40); do [ -e "/tmp/.X11-unix/X$N" ] && break; sleep 0.25; done
kill -0 "$XPID" 2>/dev/null || { echo "Xvfb did not start"; cat "$OUT/xvfb.txt"; exit 1; }
export DISPLAY=:$N SDL_VIDEO_DRIVER=x11 SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe LIBGL_DRIVERS_PATH=/usr/lib/x86_64-linux-gnu/dri LC_ALL=C.UTF-8
shot() { import -window root "$1" 2>/dev/null; }

cd "$(dirname "$BIN")" || exit 1
stdbuf -oL -eL "$BIN" --frontend sdl -B OpenGL -l "${LOGLEVEL:-2}" -r "$TID" > "$OUT/stdout.txt" 2>&1 & PID=$!
t=0; END="time limit"
while [ "$t" -lt "$SECS" ]; do
  sleep 1; t=$((t + 1))
  kill -0 "$PID" 2>/dev/null || { wait "$PID"; END="emulator exited, code $?"; PID=; break; }
  if [ -n "$UNTIL" ] && grep -aqE -- "$UNTIL" "$D/log.txt" 2>/dev/null; then END="log matched: $(grep -aE -- "$UNTIL" "$D/log.txt" | head -1 | cut -c1-120)"; sleep 5; break; fi
  if [ -n "$STOPON" ] && [ $((t % 2)) = 0 ] && tail -c 300000 "$OUT/stdout.txt" | grep -aqE -- "$STOPON"; then END="emulator message: $(tail -c 300000 "$OUT/stdout.txt" | grep -aE -- "$STOPON" | head -1 | cut -c1-160)"; sleep 2; break; fi
  [ $((t % SHOTEVERY)) = 0 ] && shot "$OUT/shots/$(printf %04d "$t").png"
  [ "$(stat -c %s "$OUT/stdout.txt")" -gt 200000000 ] && { END="log flood"; break; }
done
[ -n "$PID" ] && shot "$OUT/end.png"
cp "$D/log.txt" "$OUT/log.txt" 2>/dev/null
stopall
cp "$D/log.txt" "$OUT/log.txt" 2>/dev/null   # (again: the last lines of a log still being written when the emulator was stopped)
# (a run that ended by the app closing has no window left: its end picture is the last periodic one)
if [ "$(stat -c %s "$OUT/end.png" 2>/dev/null || echo 0)" -lt 1000 ]; then LAST=$(ls "$OUT"/shots/*.png 2>/dev/null | tail -1); [ -n "$LAST" ] && cp "$LAST" "$OUT/end.png"; fi
[ -d "$D/shots" ] && cp -r "$D/shots" "$OUT/gameshots"
[ -n "$GAME_DIR" ] && { CH=$(find "$GAME_DIR" -newer "$OUT/.start" 2>/dev/null | wc -l); [ "$CH" = 0 ] || echo "WARNING: $CH entries of the disc changed during the run" | tee -a "$OUT/run.txt"; }
cp "$XDG_CACHE_HOME/Vita3K/vita3k.log" "$OUT/vita3k.log" 2>/dev/null
grep -a -iE 'exception|error|invalid|abort|unimplemented|not found|missing|unresolved|\|C\|' "$OUT/stdout.txt" | head -300 > "$OUT/stdout.errors"
# a guest access to an unmapped address: Vita3K logs it and carries on, a real Vita stops the game (C2-12828-1). Never a pass, whatever the picture looks like.
FAULTS=$(grep -a '\[invalid_access\]' "$OUT/stdout.txt" | wc -l)
{
  [ "$FAULTS" = 0 ] || { echo "GUEST FAULTS: $FAULTS (a real Vita would crash here; this run is a FAIL)"; grep -a '\[invalid_access\]' "$OUT/stdout.txt" | sort -t']' -k2 | cut -c1-120 | uniq | head -5; }
  echo "ended after ${t}s: $END"
  echo "game log: $(wc -l < "$OUT/log.txt" 2>/dev/null || echo no) lines; screenshot: $(ls "$OUT/end.png" 2>/dev/null || ls "$OUT"/shots/*.png 2>/dev/null | tail -1)"
  echo "results: $OUT"
} | tee -a "$OUT/run.txt"
[ -s "$OUT/log.txt" ]
