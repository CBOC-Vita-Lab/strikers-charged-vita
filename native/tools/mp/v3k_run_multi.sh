#!/bin/bash
# COPY of NPV3's platform/vita/tools/v3k_run.sh with a configurable X display (V3K_DNUM) so that two emulators can run at once (tools/mp/v3k_pair.sh).
# Runs a package of the native port in the Vita3K emulator of this container, in a PRIVATE profile
# (/home/claude/msc/build/np/NPV/v3k: its own ux0 and ur0, the emulator's firmware-less modules
# shared read-only with workstream V's profile), without kubridge unless KUBRIDGE=1.
#   tools/heavy.sh -t 600 platform/vita/tools/v3k_run.sh <vpk> <title id> <seconds> [UNTIL pattern]
# e.g. ... v3k_run.sh build/np/NPV/vita/np/np-smoke.vpk MSCN00002 120 "===== done"
# Environment: ENVTXT=<file> becomes ux0:data/mscharged/env.txt. Result: <profile>/log.txt (the game's own
# log), <profile>/stdout.txt (the emulator's), a screenshot of the end. Kills what it started.
# NPV2 additions:
#   PROFILE_DIR=<dir>   the private profile (default build/np/NPV3/v3k)
#   GAME=1              ux0:data/mscharged/game -> the extracted disc of this container (the game build needs it)
#   FILES="a b"         files copied into ux0:data/mscharged (nav scripts ...)
#   SHOTEVERY=<s>       a screenshot of the emulator window every s seconds: <profile>/shots/<seconds>.png
#   ENVNAME=np-player.txt  the name ENVTXT is stored under (default env.txt); POISON=<file> is stored as env.txt as well
#   KEEPDATA=1          do not clear ux0:data/mscharged (log, nolab and env are still replaced)
#   STOPON=<egrep>     end the run when the emulator's own output matches it (default: a critical message |C|, e.g. a jump to 0, or the app closing/relaunching itself)
#   V3K_ARGS="--app-args=-settings"  extra arguments of the emulator (the application's own arguments: --app-args=...)  [W5ONE]
#   FRESHSHADERS=0      keep the emulator's shader cache of the title (default: emptied before the run: it traps stale results)
# The renderer needs Sony's shader compiler (libshacccg.suprx, the owner's, kept in workstream V's profile): linked in.
set -u
VPK=${1:?vpk}; TID=${2:?title id}; SECS=${3:-120}; UNTIL=${4:-}
# NPV3: the heavy queue is deep (an hour between two turns); a run that is already queued can still be given more or less time (the wrapper's limit
# is the job's own -t): echo <seconds> > <build root>/secs.override (read once at the start of this script, then renamed to .used).
OV=$(dirname "${PROFILE_DIR:-/home/claude/msc/build/np/NPV3/v3k}")/secs.override
[ -f "$OV" ] && { SECS=$(cat "$OV"); mv "$OV" "$OV.used"; echo "== SECS overridden to $SECS"; }
V=/home/claude/msc/v3k
P=${PROFILE_DIR:-/home/claude/msc/build/np/NPV3/v3k}
mkdir -p "$P"
if [ ! -d "$P/xdg" ]; then
  cp -a $V/xdg "$P/xdg"
  sed -i "s|^pref-path:.*|pref-path: $P/pref/|" "$P/xdg/config/Vita3K/config.yml"
  mkdir -p "$P/pref"
  for d in $V/pref/*; do n=$(basename "$d"); case $n in ux0|ur0) ;; *) ln -sfn "$d" "$P/pref/$n";; esac; done
  mkdir -p "$P/pref/ur0/tai" "$P/pref/ur0/data"
  : > "$P/pref/ur0/tai/config.txt"
fi
[ -e "$V/pref/ur0/data/libshacccg.suprx" ] && mkdir -p "$P/pref/ur0/data" && ln -sfn "$V/pref/ur0/data/libshacccg.suprx" "$P/pref/ur0/data/libshacccg.suprx"
[ "${FRESHSHADERS:-1}" = 1 ] && rm -rf "$P/xdg/cache/Vita3K/shaders/$TID" "$P/xdg/cache/Vita3K/shaderlog"
mkdir -p "$P/shots"; rm -f "$P"/shots/*.png "$P/log.best"
if [ "${KUBRIDGE:-0}" = 1 ]; then
  cp $V/pref/ur0/tai/kubridge.skprx $V/pref/ur0/tai/config.txt "$P/pref/ur0/tai/"
else
  rm -f "$P/pref/ur0/tai/kubridge.skprx"; : > "$P/pref/ur0/tai/config.txt"
fi
rm -rf "$P/pref/ux0/app/$TID"; mkdir -p "$P/pref/ux0/app/$TID" "$P/pref/ux0/data/mscharged"
if [ "${KEEPDATA:-0}" != 1 ]; then
  find "$P/pref/ux0/data/mscharged" -mindepth 1 -maxdepth 1 ! -name game ! -name nand -exec rm -rf {} + 2>/dev/null
fi
if [ "${GAME:-0}" = 1 ]; then ln -sfn /home/claude/msc/game "$P/pref/ux0/data/mscharged/game"; else rm -f "$P/pref/ux0/data/mscharged/game"; fi
for f in ${FILES:-}; do cp "$f" "$P/pref/ux0/data/mscharged/"; done
unzip -q -o "$VPK" -d "$P/pref/ux0/app/$TID"
rm -f "$P/pref/ux0/data/mscharged/log.txt" "$P/pref/ux0/data/mscharged/lab.txt"
if [ -n "${ENVTXT:-}" ]; then cp "$ENVTXT" "$P/pref/ux0/data/mscharged/${ENVNAME:-env.txt}"; else rm -f "$P/pref/ux0/data/mscharged/env.txt"; fi
# ENVNAME=np-player.txt: the settings file of the player build; POISON=<file>: put as env.txt next to it (the player build must not read it)
[ -n "${POISON:-}" ] && cp "$POISON" "$P/pref/ux0/data/mscharged/env.txt"
# lab mode would wait for a client that never comes: the emulator runs ordinary starts
: > "$P/pref/ux0/data/mscharged/nolab"
export XDG_CONFIG_HOME=$P/xdg/config XDG_CACHE_HOME=$P/xdg/cache XDG_DATA_HOME=$P/xdg/data
export APPDIR=$V/app DISPLAY=:${V3K_DNUM:-78} LIBGL_ALWAYS_SOFTWARE=1 SDL_AUDIODRIVER=dummy LC_ALL=C.UTF-8 QT_QPA_PLATFORM=xcb
cleanup() { [ -n "${PID:-}" ] && kill $PID 2>/dev/null; sleep 1; [ -n "${PID:-}" ] && kill -9 $PID 2>/dev/null; [ -n "${XPID:-}" ] && kill $XPID 2>/dev/null; }
trap cleanup EXIT INT TERM
Xvfb :${V3K_DNUM:-78} -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 & XPID=$!
sleep 1.5
cd "$V"
# One emulator run (a "phase"): starts Vita3K, waits for UNTIL / a critical message / the time, keeps the longest game log.
phase() {
  SECS=$1; UNTIL=$2
  LD_PRELOAD=$V/nobuf.so $V/app/usr/bin/Vita3K -r "$TID" ${V3K_ARGS:-} > "$P/stdout.txt" 2>&1 & PID=$!
  t=0
  while [ $t -lt "$SECS" ]; do
    sleep 1; t=$((t+1))
    kill -0 $PID 2>/dev/null || { echo "Vita3K exited at ${t}s"; break; }
    if [ $((t % 3)) = 0 ] && tail -c 300000 "$P/stdout.txt" 2>/dev/null | grep -aqE -- "${STOPON:-\|C\||in-process app relaunch|Game closed}"; then echo "emulator message at ${t}s: $(tail -c 300000 "$P/stdout.txt" | grep -aE -- "${STOPON:-\|C\||in-process app relaunch|Game closed}" | head -1 | cut -c1-160)"; sleep 1; import -window root "$P/shots/stop.png" 2>/dev/null; break; fi
    # the log of a process that was replaced by a relaunch is overwritten: keep the longest one seen
    L="$P/pref/ux0/data/mscharged/log.txt"; if [ -f "$L" ] && [ "$(stat -c %s "$L")" -ge "$(stat -c %s "$P/log.best" 2>/dev/null || echo 0)" ]; then cp "$L" "$P/log.best"; fi
    if [ -n "${SHOTEVERY:-}" ] && [ $((t % SHOTEVERY)) = 0 ]; then import -window root "$P/shots/$(printf %04d $t).png" 2>/dev/null; fi
    if [ -n "$UNTIL" ] && grep -q -- "$UNTIL" "$P/pref/ux0/data/mscharged/log.txt" 2>/dev/null; then echo "UNTIL pattern seen at ${t}s"; sleep "${UNTIL_EXTRA:-3}"; break; fi
    if [ $((t % 2)) = 0 ] && [ "$(stat -c %s "$P/stdout.txt" 2>/dev/null || echo 0)" -gt 100000000 ]; then echo "LOG FLOOD, ending"; break; fi
  done
  import -window root "$P/end.png" 2>/dev/null
  cp "$P/pref/ux0/data/mscharged/log.txt" "$P/log.txt" 2>/dev/null
  if [ -f "$P/log.best" ] && [ "$(stat -c %s "$P/log.best")" -gt "$(stat -c %s "$P/log.txt" 2>/dev/null || echo 0)" ]; then cp "$P/log.best" "$P/log.txt"; fi
  rm -f "$P/log.best"
  echo "done after ${t}s; game log: $P/log.txt ($(wc -l < "$P/log.txt" 2>/dev/null) lines)"
}
phase "$SECS" "$UNTIL"
# (the phase-2 mechanism of the original is not needed for a pair run)
