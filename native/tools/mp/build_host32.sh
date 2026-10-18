#!/bin/bash
# Builds np-host32-full-null (vgx null back end: no pictures, no Mesa) into build/np/NPM2. Run through heavy.sh:
#   /home/claude/msc/tools/heavy.sh -t 1500 tools/mp/build_host32.sh
# A fresh build root is seeded from build/np/host32 with `tar` (see npw/NPM2.NOTES.md); sources are aged so only changed files rebuild.
cd "$(dirname "$0")/../.."
BR=${BUILD_ROOT:-/home/claude/msc/build/np/NPM2}
make -j2 TARGET=host32 WITH_SDK=1 WITH_GX=1 WITH_PLAT=1 OPT=-O1 DEBUG=-g0 EXESUF=${EXESUF:--full-null} GX_BACKEND=${GX_BACKEND:-null} BUILD_ROOT=$BR "$@" 2>&1 | grep -v -E 'overriding recipe|ignoring old recipe' | tail -${TAILN:-40}
echo "build rc=${PIPESTATUS[0]}"
