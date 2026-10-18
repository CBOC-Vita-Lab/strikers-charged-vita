#!/bin/bash
# The native port on the console, from a PC or Mac that can reach the Vita over Wi-Fi (NPV3; replaces NPV2's session_np1.sh).
#
# ROUTE B (default): the lab build is INSTALLED as title MSCN00001 through vitacompanion's `promote` (step 0), started with `launch`, and
# is then the lab monitor itself: every run is a `cycle` without --exec. New builds go in as `put eboot-np-lab.bin app0:lab-new.bin` +
# `--exec lab-new.bin` (UPDATE=1). The owner touches nothing.
# ROUTE A (ROUTE=A): the recompiled build's lab application (MSCH00002, lab server, kubridge) is already running; the native executable is
# put into ITS folder as np-new.bin and started with --exec. Nothing is installed.
#
#   cd <kit directory>
#   export VITA_HOST=<your-console-address>
#   bash session_np2.sh              # steps 0-9
#   bash session_np2.sh 1 3 7        # only these steps
# Steps: 0 install, 1 ping, 2 put (route A / UPDATE=1), 3 boot check, 4 crash, 5 hang, 6 boot, 7 boot to title, 8 timed match, 9 smoke, F a whole match, AB the match with the other code flavour, 10 player build.
# Every vitalab.py command that ends with status 4 (budget used up, not failed) is simply given again (rep).
set -u
cd "$(dirname "$0")"
L="python3 vitalab.py"
ROUTE=${ROUTE:-B}
X=""; EBOOT_REMOTE=app0:np-new.bin
if [ "$ROUTE" = A ]; then X="--exec np-new.bin"; fi
if [ "${UPDATE:-0}" = 1 ] && [ "$ROUTE" = B ]; then X="--exec lab-new.bin"; EBOOT_REMOTE=app0:lab-new.bin; fi
EB=${EBOOT:-eboot-np-lab.bin}
rep() {   # repeat a command while it answers "not finished" (status 4), at most 12 times
  local n=0 rc
  while :; do "$@"; rc=$?; [ $rc = 4 ] || return $rc; n=$((n+1)); [ $n -ge 12 ] && { echo "gave up after $n repeats"; return 4; }; done
}
step() { echo; echo "=== step $1: $2 ($(date +%T))"; }
STEPS=("$@")
sel() { [ ${#STEPS[@]} -eq 0 ] && return 0; for s in "${STEPS[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1; }

# 0 install (route B only)
sel 0 && [ "$ROUTE" = B ] && { step 0 "install the lab build as MSCN00001 (vitacompanion: FTP upload, promote, launch)"
           bash install_promote.sh lab || exit 1; }
# 1 ping
sel 1 && { step 1 "the lab server answers (expect: msclab proto=2 ... variant=np title=$([ "$ROUTE" = B ] && echo MSCN00001 || echo MSCH00002) ...)"; $L ping || exit 1; }
# 2 put (route A, or UPDATE=1)
sel 2 && [ -n "$X" ] && { step 2 "upload the native executable ($EBOOT_REMOTE, about 6 MB; repeat on status 4)"
           rep $L put "$EB" "$EBOOT_REMOTE" || exit 1; $L stat "$EBOOT_REMOTE"; }
sel 3 && { step 3 "boot check: 300 frames, then the harness exits and the lab server is back (expect: run: ended: reached MSC_MAX_FRAMES=300)"
           rep $L cycle env/env-np-frames.txt np-chk1 150 $X; echo "rc=$?"
           grep -h "native port\|renderer start-up\|full screen\|\[lab\] RUN\|stage:\|first frame\|MAX_FRAMES\|FATAL\|CRASH" np-chk1/log.txt 2>/dev/null | head -20; }
sel 4 && { step 4 "crash handler and watchdog: a null read at frame 200 (expect: ended: CRASH: data abort ...)"
           rep $L cycle env/env-np-crash.txt np-chk2 150 $X; echo "rc=$?"; }
sel 5 && { step 5 "hang detection: the game thread spins from frame 200 (expect: ended: stall: no frame presented for 30 s)"
           rep $L cycle env/env-np-hang.txt np-chk3 150 $X; echo "rc=$?"; }
sel 6 && { step 6 "BOOT: boot, intro movie scene, ESRB screen (2400 frames or 600 s)"
           rep $L cycle env/env-np-boot.txt np-boot 600 $X; echo "rc=$?"
           grep -h "\[nav\]\|\[frame-timing\]\|\[mem\]" np-boot/log.txt 2>/dev/null | tail -40; }
sel 7 && { step 7 "BOOT TO TITLE: the nav script presses A to the title screen (pad path: script -> raw pad -> the emulated Wii Remote)"
           rep $L cycle env/env-np-title.txt np-title 600 $X --input nav-native/boot_title.txt; echo "rc=$?"
           grep -h "\[script\]\|\[nav\]" np-title/log.txt 2>/dev/null | tail -20; }
sel 8 && { step 8 "TIMED SCRIPTED MATCH: menus to a kick-off and 62 s of play in real time, with the frame statistics (about 3-6 minutes)"
           rep $L cycle env/env-np-match.txt np-match 900 $X --input nav/match60.txt; echo "rc=$?"
           grep -h "\[script\]\|\[nav\]\|\[watch\]\|\[frame-timing\]\|\[mem\]" np-match/log.txt 2>/dev/null | tail -60; }
sel 9 && { step 9 "PLATFORM SMOKE: core scaling, job pool, audio, frames (no game; the numbers the 30 fps plan rests on)"
           rep $L put eboot-np-smoke.bin app0:np-smoke.bin || exit 1
           rep $L cycle env/env-np-smoke.txt np-smoke 240 --exec np-smoke.bin; echo "rc=$?"
           grep -h "\[smoke\]" np-smoke/log.txt 2>/dev/null | head -60; }
# F one whole match from the ESRB screen to the rematch (about 10 minutes; the longest run of the kit)
sel F && [ ${#STEPS[@]} -gt 0 ] && { step F "ONE WHOLE MATCH, real time (menus, kick-off, play to full time, results, rematch): about 10 minutes"
           rep $L cycle env/env-np-fullmatch.txt np-full 1500 $X --input nav/fullmatch.txt; echo "rc=$?"
           python3 runlog_summary.py np-full/log.txt --every 2 2>/dev/null | tail -50; }
# AB the other code flavour: eboot-np-lab.bin is ThinLTO -O2, eboot-np-lab-o2.bin is -O2 without LTO (same sources): the same timed match with each is the A/B
sel AB && [ ${#STEPS[@]} -gt 0 ] && { step AB "the timed match again with the -O2 (no LTO) executable: put as lab-o2.bin, cycle --exec lab-o2.bin"
           rep $L put eboot-np-lab-o2.bin app0:lab-o2.bin || exit 1
           rep $L cycle env/env-np-match.txt np-match-o2 900 --exec lab-o2.bin --input nav/match60.txt; echo "rc=$?"
           grep -h "\[script\]\|\[watch\]\|\[frame-timing\]" np-match-o2/log.txt 2>/dev/null | tail -40; }
# 10 the player build (only when asked: it is a second bubble, MSCN00003, with no lab server)
sel 10 && [ ${#STEPS[@]} -gt 0 ] && { step 10 "install the PLAYER build (MSCN00003, an ordinary game: no lab, no scripts) and launch it"
           bash install_promote.sh player || exit 1; }
echo; echo "=== done $(date +%T). Send back: np-chk1 np-chk2 np-chk3 np-boot np-title np-match np-smoke (each: log.txt = the run, state.txt = how it ended, lab.txt = the server's own log, shots/)"
