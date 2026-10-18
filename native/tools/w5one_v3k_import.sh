#!/bin/bash
# W5ONE: the first-start save import of the player package in Vita3K.
#   tools/heavy.sh -t 400 tools/w5one_v3k_import.sh <vpk> <title id>        Result: build/np/W5ONE/v3k-import/ (log.txt, verdict.txt, end.png)
# The emulator's ux0:data/mscharged/NAND holds a populated save in the recompiled build's layout (made from this machine's real fresh save by tests/test_save_import
# "populate"); the native data directory (np-data) is empty. The player package starts, imports (log line "[vita] save import:"), the nav script
# tools/nav/nps2_savetest.txt walks title -> main menu: with a save there must be no "create save data" prompt ("first run" line absent, "STATE main menu with a save" present).
VPK=${1:?vpk}; TID=${2:-MSCN00003}
cd "$(dirname "$0")/.."
NP=$PWD; B=/home/claude/msc/build/np/W5ONE; R=$B/v3k-import; P=$B/v3k_i
rm -rf $R $P; mkdir -p $R/files
D=$P/pref/ux0/data/mscharged; N=$D/NAND/title/00010000/52345145/data; mkdir -p $N
REAL=/home/claude/msc/appdata/MSCRecomp/NAND/title/00010000/52345145/data
T=$B/set/test_save_import
[ -x $T ] || g++ -std=gnu++17 -O1 -w $NP/platform/sdk/tests/test_save_import.cpp -o $T || exit 1
$T populate $REAL/Strikers2 $N/Strikers2 || exit 1
cp $REAL/banner.bin $N/banner.bin
touch -d '2026-01-02 03:04:05' $N/Strikers2 $N/banner.bin
before="$(sha256sum $N/Strikers2 | cut -c1-16) $(stat -c %Y $N/Strikers2) $(stat -c %s $N/Strikers2)"
cp $N/Strikers2 $R/source.Strikers2
cp tools/nav/nps2_savetest.txt $R/files/
cat > $R/np-player.txt <<EOT
# test np-player.txt of the save import run
MSC_INPUT_SCRIPT=nps2_savetest.txt
MSC_MAX_FRAMES=6000
MSC_WATCH=1
EOT
export PROFILE_DIR=$P V3K_DNUM=83 GAME=1 ENVNAME=np-player.txt ENVTXT=$R/np-player.txt SHOTEVERY=30 KEEPDATA=1
FILES="$R/files/nps2_savetest.txt" tools/mp/v3k_run_multi.sh "$VPK" "$TID" 330 "STATE main menu with a save" > $R/run.txt 2>&1
cp $D/log.txt $R/log.txt 2>/dev/null; cp $P/end.png $R/ 2>/dev/null; cp -r $P/shots $R/shots 2>/dev/null
head -c 100000 $P/stdout.txt > $R/stdout.head 2>/dev/null
NN=$D/np-data/NAND/title/00010000/52345145/data
after="$(sha256sum $N/Strikers2 | cut -c1-16) $(stat -c %Y $N/Strikers2) $(stat -c %s $N/Strikers2)"
{
  echo "run: $(tail -2 $R/run.txt | tr '\n' ' ')"
  echo "source before: $before"; echo "source after:  $after  -> $( [ "$before" = "$after" ] && echo UNTOUCHED || echo CHANGED )"
  ls -l --time-style=full-iso $NN 2>&1
  echo "native copy equals the source (first run of the game may have re-saved it): $(cmp -s $N/Strikers2 $NN/Strikers2 && echo identical || echo differs)"
  grep -a -E "save import|first run|STATE|accepting|np-player.txt" $R/log.txt | cut -c1-200 | head -20
} | tee $R/verdict.txt
