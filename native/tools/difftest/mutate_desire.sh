#!/bin/bash
# mutate_desire.sh START COUNT : apply lines START..START+COUNT-1 of mutations_desire.txt ("name|sed-expr") to a PRIVATE COPY of Desire.cpp,
# compile it to a private object, relink dt_b_mut and compare the Desire tests (200 seeds) with out_da.txt. The real source is never touched.
DT=$(cd "$(dirname "$0")" && pwd)
START=${1:-1}; COUNT=${2:-100}
if [ -f "$DT/../../src/Game/AI/Desire.cpp" ]; then cd "$DT/../.."; else cd /home/claude/msc/npw/NPG; fi
BR=${BUILD_ROOT:-/home/claude/msc/build/np/NPG}
MUT=$BR/dt/mut; mkdir -p $MUT
O=$BR/host32/obj/src
cmd=$(make -n TARGET=host32 BUILD_ROOT=/tmp/claude-0/xx /tmp/claude-0/xx/host32/obj/src/Game/AI/Desire.cpp.o 2>&1 | grep -o "clang++ .*Desire.cpp" | head -1 | sed 's/ -MMD.*//; s| [^ ]*Desire.cpp$||')
OBJS="$BR/dt/dt_b.o $O/Game/AI/SpaceSearch.cpp.o $O/Game/AI/AiUtil.cpp.o $O/Game/AI/Fuzzy.cpp.o $O/Game/AI/shdStateMachine.cpp.o $O/NL/nlTimer.cpp.o $O/Game/AI/TransitionFunc.cpp.o $O/Game/AI/FuzzyVariant.cpp.o $O/Game/AI/Variant.cpp.o"
NAMES=CUT_UPDATE,GETOPEN_INIT,RUN_UPDATE,RUN_INIT,INTERCEPT_INIT,INTERCEPT_UPDATE,TRANS_RUN,TRANS_GETOPEN,WEATHER
sed -n "${START},$((START+COUNT-1))p" "$DT/mutations_desire.txt" | while IFS='|' read -r name expr; do
  sed "$expr" src/Game/AI/Desire.cpp > $MUT/Desire.cpp
  if cmp -s $MUT/Desire.cpp src/Game/AI/Desire.cpp; then echo "$name: MUTATION DID NOT APPLY"; continue; fi
  if ! $cmd -c $MUT/Desire.cpp -Isrc/Game/AI -o $MUT/Desire.o 2> $MUT/err.txt; then echo "$name: BUILD FAIL $(head -c 200 $MUT/err.txt)"; continue; fi
  clang++ -m32 -static -no-pie -o $MUT/dt_b_mut $OBJS $MUT/Desire.o -Wl,--unresolved-symbols=ignore-all -lm 2>/dev/null
  rm -f $MUT/out.txt
  for t in ${NAMES//,/ }; do DT_ONLY=$t timeout 60 $MUT/dt_b_mut 0 200 desire >> $MUT/out.txt 2>/dev/null; done
  res=$($DT/cmpd.py $DT/out_da.txt $MUT/out.txt 2e-4 $NAMES | tail -1)
  echo "$name: $res"
done
