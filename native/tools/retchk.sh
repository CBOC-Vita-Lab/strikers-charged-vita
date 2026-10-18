#!/bin/bash
# Falling off the end of a non-void function is undefined behaviour in C++ and clang/gcc then run into the next
# function (Metrowerks returned whatever was in r3). Lists every such function (clang -Wreturn-type, host32).
#   usage (inside a heavy.sh slot): tools/retchk.sh   -> /home/claude/msc/build/np/retchk/report.txt
cd /home/claude/msc/np
O=/home/claude/msc/build/np/retchk
rm -rf $O; mkdir -p $O
make -k -j2 TARGET=host32 BUILD_ROOT=$O WARN="-Wreturn-type" syntax > $O/syn.log 2>&1
find $O/host32/log -name '*.err' -print0 | xargs -0 grep -h "warning: .*non-void\|warning: non-void" 2>/dev/null | sort | uniq > $O/report.txt
wc -l < $O/report.txt
head -80 $O/report.txt
