#!/bin/bash
# W5ONE: the Settings screen of the player package in Vita3K, in three phases of one heavy turn:
#   A  the marker file np-settings-open (its words are the keys: platform/vita/src/np_vita_settings.cpp) opens the screen, changes the preset, the multiplayer link and the
#      original game fixes, takes screenshots (the screen's own, ux0:data/mscharged/shots/settings-*.ppm) and does "Save and exit": np-player.txt must hold the new block and keep the other lines
#   B  the screen opens again (values read back from the file), goes to "Save and start": the application starts again (sceAppMgrLoadExec) and the game boots with the new
#      variables (log lines "[vita] np-player.txt: NAME=VALUE")
#   tools/heavy.sh -t 590 tools/w5one_v3k_settings.sh <vpk> <title id>      Result: build/np/W5ONE/v3k-settings/{A,B}/ (log.txt, np-player.txt, shots, stdout.head)
VPK=${1:?vpk}; TID=${2:-MSCN00003}
cd "$(dirname "$0")/.."
B=/home/claude/msc/build/np/W5ONE; R=$B/v3k-settings; rm -rf $R; mkdir -p $R/A $R/B $R/files
cat > $R/np-player.txt <<EOT
# test np-player.txt of the Settings screen run (a comment and two variables of the tester's own: they must survive)
MSC_WATCH=1
MSC_INPUT_SCRIPT=boot_title.txt
MSC_MAX_FRAMES=2500
NP_DATA_DIR=ux0:data/mscharged/np-data
EOT
cp tools/nav/boot_title.txt $R/files/
# rows: 0 preset, 1 resolution, 2 performance, 3 pacing, 4 thread plan, 5 sharpness, 6 anti-aliasing, 7 picture format, 8 link, 9 fixes, 10-13 controls, 14 reset, 15 save and start, 16 save and exit
# (checked with a model of the screen's key handling: the sequences end on the intended rows; "up" from row 0 would wrap to 16)
echo "wait:1500 shot:1-open right shot:2-quality right shot:3-performance down down down down down down down down right down right right shot:4-link-fixes down down down down down down down shot:5-save-exit cross" > $R/files/np-settings-open
export PROFILE_DIR=$B/v3k_s V3K_DNUM=82 GAME=1 ENVNAME=np-player.txt ENVTXT=$R/np-player.txt SHOTEVERY=4
FILES="$R/files/boot_title.txt $R/files/np-settings-open" tools/mp/v3k_run_multi.sh "$VPK" "$TID" 120 "settings screen done" > $R/A/run.txt 2>&1
P=$PROFILE_DIR; D=$P/pref/ux0/data/mscharged
cp $D/np-player.txt $R/A/ ; cp $D/log.txt $R/A/ 2>/dev/null; cp -r $D/shots $R/A/gameshots 2>/dev/null; cp -r $P/shots $R/A/shots; head -c 100000 $P/stdout.txt > $R/A/stdout.head
echo "== A: $(tail -2 $R/A/run.txt | tr '\n' ' ')"; echo "-- np-player.txt after A:"; cat $R/A/np-player.txt
# B: the screen again (it reads the file back), then Save and start; the marker is gone after A, put it back; KEEPDATA keeps np-player.txt (ENVTXT is not copied again).
#    Vita3K logs the call and then closes the game (its -r mode has no shell to start the application again): the run ends at that message.
echo "wait:1500 shot:6-reopened down down down down down down down down down down down down down down down shot:7-save-start cross" > $R/files/np-settings-open
unset ENVTXT
KEEPDATA=1 FILES="$R/files/np-settings-open" tools/mp/v3k_run_multi.sh "$VPK" "$TID" 90 "NEVER-MATCHES-ANYTHING" > $R/B/run.txt 2>&1
cp $D/np-player.txt $R/B/ ; cp $D/log.txt $R/B/ 2>/dev/null; cp -r $D/shots $R/B/gameshots 2>/dev/null; cp -r $P/shots $R/B/shots; head -c 100000 $P/stdout.txt > $R/B/stdout.head
echo "== B: $(tail -2 $R/B/run.txt | tr '\n' ' ')"
grep -a -E "\[settings\]|np-player.txt|variable\(s\) set" $R/B/log.txt | cut -c1-170 | head -24
grep -a -i "LoadExec\|relaunch" $R/B/stdout.head | head -5 | cut -c1-200
# C: the start argument -settings (what a LiveArea start with the Settings parameter would pass, if it comes as an argument) opens the screen without the marker file
KEEPDATA=1 V3K_ARGS="--app-args=-settings" tools/mp/v3k_run_multi.sh "$VPK" "$TID" 60 "\[settings\] screen open" > $R/C.run.txt 2>&1
mkdir -p $R/C; cp $D/log.txt $R/C/ 2>/dev/null
echo "== C (--app-args=-settings): $(tail -1 $R/C.run.txt)"; grep -a -E "\[settings\]" $R/C/log.txt | head -4 | cut -c1-170
# D: the start argument -play (what Save and start passes) must NOT open the screen even with the marker file there; the game goes on to its start-up
echo "wait:1500 shot:8-must-not-open" > $R/files/np-settings-open
KEEPDATA=1 V3K_ARGS="--app-args=-play" FILES="$R/files/np-settings-open" tools/mp/v3k_run_multi.sh "$VPK" "$TID" 60 "stage: the game's start-up" > $R/D.run.txt 2>&1
mkdir -p $R/D; cp $D/log.txt $R/D/ 2>/dev/null
echo "== D (--app-args=-play, marker present): $(tail -1 $R/D.run.txt)"; grep -a -E "\[settings\]|stage: the game's start-up" $R/D/log.txt | head -4 | cut -c1-170
rm -f $D/np-settings-open
