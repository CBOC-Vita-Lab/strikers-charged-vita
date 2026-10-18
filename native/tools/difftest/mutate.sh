#!/bin/bash
# mutate.sh 'sed-expr' : apply to SpaceSearch.cpp, rebuild dt_b to dt_b_mut, compare 100 seeds, restore
DT=$(cd "$(dirname "$0")" && pwd); cd "$DT"; export DT
S=$DT/../../src/Game/AI/SpaceSearch.cpp
cp /tmp/claude-0/SpaceSearch.cpp.orig $S
sed -i "$1" $S
if cmp -s $S /tmp/claude-0/SpaceSearch.cpp.orig; then echo "MUTATION DID NOT APPLY: $1"; exit 1; fi
BIN=$DT/dt_b_mut $DT/build_b.sh > /tmp/claude-0/mut.log 2>&1 || { echo "BUILD FAIL"; tail -3 /tmp/claude-0/mut.log; cp /tmp/claude-0/SpaceSearch.cpp.orig $S; exit 1; }
./dt_b_mut 0 100 > out_m.txt 2>/dev/null
./cmp.py out_a.txt out_m.txt | tail -1 | sed "s|^|$1  ==> |"
cp /tmp/claude-0/SpaceSearch.cpp.orig $S
