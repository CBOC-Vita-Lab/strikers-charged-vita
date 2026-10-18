#!/bin/bash
# tools/v3k_ch_run.sh <tag> <seconds> [shot-every-seconds] -- <Vita3K args>
# One Vita3K run in the renderer workstream's PRIVATE emulator profile
# (/home/claude/msc/build/host-gl-v3k/{pref,xdg}: its own memory card and emulator caches, so
# that runs of other workers on v3k/pref are not disturbed; ur0 is a link to v3k/pref/ur0).
# ALWAYS through the heavy lock, without -p:
#   /home/claude/msc/tools/heavy.sh -t 600 tools/v3k_ch_run.sh seed1 420 60 -- -r MSCH00002 -l 2
# A copy of v3k/run.sh reduced to what is used here. UNTIL="<grep pattern>" ends the run 3 s
# (UNTIL_EXTRA) after the game's log.txt contains the pattern. Kills only the processes it started.
# Result: $P/shots/<tag>-<sec>.png, $P/logs/<tag>.{stdout,v3k.log,log.txt}
P=/home/claude/msc/build/host-gl-v3k; V=/home/claude/msc/v3k
. $V/env.sh
export XDG_CONFIG_HOME=$P/xdg/config XDG_CACHE_HOME=$P/xdg/cache XDG_DATA_HOME=$P/xdg/data
export DISPLAY=:78
tag=$1; secs=$2; shift 2
every=15
if [ "$1" != "--" ]; then every=$1; shift; fi
shift
xpid=""; pid=""
cleanup() { [ -n "$pid" ] && kill $pid 2>/dev/null; sleep 1; [ -n "$pid" ] && kill -9 $pid 2>/dev/null; [ -n "$xpid" ] && kill $xpid 2>/dev/null; }
trap cleanup EXIT INT TERM
Xvfb $DISPLAY -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 &
xpid=$!
sleep 1.5
rm -f $XDG_CACHE_HOME/Vita3K/vita3k.log
G=$P/pref/ux0/data/mscharged
mv -f $G/log.txt $P/logs/prev.log.txt 2>/dev/null
cd $P
LD_PRELOAD=$V/nobuf.so $V3K "$@" > logs/$tag.stdout 2>&1 &
pid=$!
t=0
while [ $t -lt $secs ]; do
  sleep 1; t=$((t+1))
  kill -0 $pid 2>/dev/null || { echo "Vita3K exited at ${t}s (status $(wait $pid; echo $?))"; break; }
  if [ -n "$UNTIL" ] && grep -q -- "$UNTIL" $G/log.txt 2>/dev/null; then
    echo "UNTIL pattern seen at ${t}s"; sleep ${UNTIL_EXTRA:-3}; import -window root shots/$tag-until.png 2>/dev/null; break
  fi
  if [ $((t % 2)) = 0 ] && [ "$(stat -c %s logs/$tag.stdout 2>/dev/null || echo 0)" -gt 150000000 ]; then
    echo "LOG FLOOD at ${t}s, ending the run"; kill -9 $pid
    head -c 4000000 logs/$tag.stdout > logs/$tag.stdout.head; mv -f logs/$tag.stdout.head logs/$tag.stdout
    rm -f $XDG_CACHE_HOME/Vita3K/vita3k.log; break
  fi
  if [ $((t % every)) = 0 ]; then import -window root shots/$tag-$(printf %04d $t).png 2>/dev/null; fi
done
import -window root shots/$tag-end.png 2>/dev/null
cp $XDG_CACHE_HOME/Vita3K/vita3k.log logs/$tag.v3k.log 2>/dev/null
cp $G/log.txt logs/$tag.log.txt 2>/dev/null
cleanup
trap - EXIT
sleep 0.5
echo "run $tag done after ${t}s"
