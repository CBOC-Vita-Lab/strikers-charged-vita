#!/bin/bash
# build.sh: the live differential tester.  Needs a host32 full-null build (BUILD_ROOT) whose Goalie.cpp was compiled with -DNP_DTL.
#   tools/dtlive/build.sh [BUILD_ROOT]      -> $BUILD_ROOT/host32/np-host32-dtl
# 1. gen_dtl.py from the symbols of the plain executable  2. dtl.cpp + generated translation  3. relink with dtl.o
set -e
HERE=$(cd "$(dirname "$0")" && pwd); NP=$(cd "$HERE/../.." && pwd)
BR=${1:-/home/claude/msc/build/np/NPG2}
D=$BR/dtl; mkdir -p $D/out
EXE=$BR/host32/np-host32-full-null
RT=$D/rt
if [ ! -d $RT ]; then
  cp -r /home/claude/msc/one/port/runtime/include $RT
  sed -i 's/defined(__x86_64__)/(defined(__x86_64__) || defined(__i386__))/g' $RT/isa/*.h $RT/*.h
fi
[ -f $NP/tools/difftest/index.pkl ] || python3 $NP/tools/difftest/mkindex.py
python3 $HERE/gen_bf.py $EXE $D/out
python3 $HERE/gen_dtl.py $EXE $D/out ${GENARGS:---roots Game/Goalie.cpp,Game/AI/GoalieActions.cpp,Game/AI/GoalieLooseBall.cpp,Game/AI/GoalieSave.cpp,Game/GoalieFatigue.cpp,Game/Render/PeachPhoto.cpp,Game/PoseAccumulator.cpp} --sweep
clang++ -m32 -msse2 -mfpmath=sse -mfma -ffp-contract=off -std=c++20 -O1 -w -fno-strict-aliasing -isystem /usr/include/x86_64-linux-gnu \
    -I $HERE/shim -I $RT -I $D/out -c $HERE/dtl.cpp -o $D/dtl.o
# relink: the game's link line plus dtl.o
cd $NP
make -n -W $BR/host32/pobj/platform/compat/np_main.cpp.o -j2 TARGET=host32 WITH_SDK=1 WITH_GX=1 WITH_PLAT=1 OPT=-O1 DEBUG=-g0 EXESUF=-full-null GX_BACKEND=null BUILD_ROOT=$BR 2>/dev/null \
  | grep '^clang++ -o' | sed 's# 2> .*$##' | sed "s#-o $EXE#-o $BR/host32/np-host32-dtl $D/dtl.o#" > $D/link.sh
bash $D/link.sh
ls -la $BR/host32/np-host32-dtl
