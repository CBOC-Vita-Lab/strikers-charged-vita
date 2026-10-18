#!/bin/bash
# run_all.sh : build dt_a / dt_b and run the whole differential test (Desire functions, then the SpaceSearch evaluators); prints the diffs.
# Needs the recompiler's shards (mkindex.py), the retail main.dol, and the host32 objects of the np tree (build_b.sh makes them).
# Run it through tools/heavy.sh. Results land next to the scripts (out_*.txt, not committed).
DT=$(cd "$(dirname "$0")" && pwd); cd "$DT"
BR=${BUILD_ROOT:-/home/claude/msc/build/np/NPG}
O=$BR/host32/obj/src
export EXTRA_SRC="src/Game/AI/Desire.cpp src/Game/AI/shdStateMachine.cpp src/NL/nlTimer.cpp src/Game/AI/TransitionFunc.cpp src/Game/AI/FuzzyVariant.cpp src/Game/AI/Variant.cpp"
export EXTRA_OBJ="$O/Game/AI/Desire.cpp.o $O/Game/AI/shdStateMachine.cpp.o $O/NL/nlTimer.cpp.o $O/Game/AI/TransitionFunc.cpp.o $O/Game/AI/FuzzyVariant.cpp.o $O/Game/AI/Variant.cpp.o"
DOL=${DOL:-/home/claude/msc/game/sys/main.dol}
./build_b.sh 2>&1 | grep -v "no-pie" | tail -20
./build_a.sh 2>&1 | grep -E "error" | head -30
./dt_a $DOL 0 200 desire > out_da.txt 2> err_da.txt; echo "dt_a desire rc=$? lines=$(wc -l < out_da.txt)"; tail -3 err_da.txt
rm -f out_db.txt
for t in CUT_UPDATE GETOPEN_INIT RUN_UPDATE RUN_INIT INTERCEPT_INIT INTERCEPT_UPDATE TRANS_RUN TRANS_GETOPEN WEATHER; do
  DT_ONLY=$t timeout 60 ./dt_b 0 200 desire >> out_db.txt 2> err_db_$t.txt || echo "dt_b $t crashed (rc=$?)"
done
./cmpd.py out_da.txt out_db.txt | tail -50
echo "=== SpaceSearch evaluators (300 seeds)"
./dt_a $DOL 0 300 > out_a.txt 2> err_a.txt; echo "dt_a rc=$?"
./dt_b 0 300 > out_b.txt 2> err_b.txt; echo "dt_b rc=$?"
./cmp.py out_a.txt out_b.txt | tail -14
