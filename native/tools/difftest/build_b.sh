#!/bin/bash
# build dt_b: native SpaceSearch.cpp (+ Desire.cpp later) over the stand-ins
set -e
DT=$(cd "$(dirname "$0")" && pwd)
cd "$DT/../.."
BR=${BUILD_ROOT:-/home/claude/msc/build/np/NPG}
OUT=$BR/dt; mkdir -p $OUT
for f in src/Game/AI/SpaceSearch.cpp src/Game/AI/AiUtil.cpp src/Game/AI/Fuzzy.cpp ${EXTRA_SRC}; do
  make -s TARGET=host32 BUILD_ROOT=$BR $BR/host32/obj/$f.o
done
cmd=$(make -n TARGET=host32 BUILD_ROOT=/tmp/claude-0/xx /tmp/claude-0/xx/host32/obj/src/Game/AI/Desire.cpp.o 2>&1 | grep -o "clang++ .*Desire.cpp" | head -1 | sed 's/ -MMD.*//; s| [^ ]*Desire.cpp$||')
$cmd -funsigned-char -O1 -g -w -I$DT -c ${SRC:-$DT/dt_b.cpp} -o $OUT/dt_b.o
O=$BR/host32/obj/src/Game/AI
clang++ -m32 -static -no-pie -o ${BIN:-$DT/dt_b} $OUT/dt_b.o $O/SpaceSearch.cpp.o $O/AiUtil.cpp.o $O/Fuzzy.cpp.o ${EXTRA_OBJ} -Wl,--unresolved-symbols=ignore-all -lm
