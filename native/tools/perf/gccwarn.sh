#!/bin/bash
# NPP: the GCC-only hazards of a CCTOOL=gcc build: warnings of the classes that GCC turns into different code (it assumes the undefined case cannot happen).
#   MAKEWARN="-Winfinite-recursion -Warray-bounds -Wstringop-overflow -Wuninitialized -Wmaybe-uninitialized -Waggressive-loop-optimizations -Wnonnull \
#             -Wdangling-pointer -Wreturn-local-addr -Wreturn-type -Wfree-nonheap-object -Wshift-count-overflow -Wshift-count-negative -Wsequence-point -Wplacement-new" \
#   V=g13w BUILDVARS="OPT=-O2 CCTOOL=gcc NOPIC=1" tools/heavy.sh -t 1800 tools/perf/job_a.sh      (the per-TU compiler logs land in <build>/<variant>/armhf/log)
#   tools/perf/gccwarn.sh <variant>      -> counts per class, then one line per distinct warning (build paths stripped)
D=/home/claude/msc/build/np/NPP2/${1:?variant}/armhf/log
find "$D" -type f -print0 | xargs -0 cat | grep "warning:" | sed -E 's#^(\./)?##' | grep -v "invalid-offsetof\|Wwrite-strings\|Wattributes\|__CONCAT\|redefined\|typedef\|used but never defined" > /tmp/gccwarn.$$
echo "== classes"; grep -o "\[-W[a-z0-9=-]*\]" /tmp/gccwarn.$$ | sort | uniq -c | sort -rn
echo "== distinct"; sort -u /tmp/gccwarn.$$ | cut -c1-240
rm -f /tmp/gccwarn.$$
