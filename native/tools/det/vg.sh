#!/bin/bash
# The host32 null executable under valgrind memcheck: uninitialised reads in the game (the oracle's nav script, virtual time).
#   heavy.sh -t 590 tools/det/vg.sh -n name [-x exe] [-s script] [-f frames] [-o yes|no (track origins)] [-- extra valgrind options]
# Log: build/np/B3/vg-<name>.log (valgrind) and vg-<name>.out (the game). Summary: tools/det/vgsum.py.
B=/home/claude/msc/build/np; NAME=vg; EXE=$B/B3x/host32/np-host32-full-null; SCRIPT=/home/claude/msc/tools/scripts/nav_kickoff.txt; FR=3500; ORI=no
while getopts "n:x:s:f:o:" o; do case $o in n) NAME=$OPTARG;; x) EXE=$OPTARG;; s) SCRIPT=$OPTARG;; f) FR=$OPTARG;; o) ORI=$OPTARG;; *) exit 2;; esac; done
shift $((OPTIND-1))
# valgrind cannot start the system 32-bit ld.so (stripped: its strlen redirection fails): run the executable through the private i386 loader
# of build/np/B3/dbg32 (Ubuntu libc6:i386, its debug symbols copied into /usr/lib/debug/.build-id). (patchelf on the executable instead loses its symbols.)
L=$B/B3/dbg32/libroot/usr/lib/i386-linux-gnu
LDR="$L/ld-linux.so.2 --library-path $L:/lib32"
D=$B/B3/data-vg-$NAME; rm -rf $D; mkdir -p $D; cd $D
export NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D MSC_INPUT_SCRIPT=$SCRIPT MSC_MAX_FRAMES=$FR MSC_VIRTUAL_TIME=1 MSC_FRAME_LOG=100000 MSC_STATE_HASH=60
T0=$(date +%s)
valgrind --tool=memcheck --soname-synonyms=somalloc=nouse --track-origins=$ORI --error-limit=no --num-callers=14 --undef-value-errors=yes --leak-check=no --log-file=$B/B3/vg-$NAME.log "$@" $LDR $EXE > $B/B3/vg-$NAME.out 2>&1
echo "rc=$? wall=$(( $(date +%s) - T0 ))s errors: $(grep -c 'uninitialised' $B/B3/vg-$NAME.log)"; tail -3 $B/B3/vg-$NAME.out | cut -c1-160
rm -rf $D
