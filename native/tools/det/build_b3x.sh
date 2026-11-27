#!/bin/bash
# Build the host32 null variant (OPT=-O1, as NPB2's) of the current worktree into the private root build/np/B3x (copy of build/np/host32's objects).
#   heavy.sh -t 590 tools/det/build_b3x.sh [extra make vars]
cd /work/msc/np
make -j2 TARGET=host32 WITH_SDK=1 WITH_GX=1 WITH_PLAT=1 GX_BACKEND=null EXESUF=-full-null OPT=-O1 DEBUG=-g0 BUILD_ROOT=/work/msc/build/np/B3x "$@" 2>&1 | grep -v '^clang\|^mkdir\|^ar ' | tail -${TAILN:-15}
ls -la /work/msc/build/np/B3x/host32/np-host32-full-null
