#!/bin/bash
# Builds the platform smoke application (np-smoke.{elf,vpk,eboot.bin}) next to a game package: the platform layer's objects are the game package's.
#   BUILD_ROOT=/home/claude/msc/build/np/NPV3/c1 platform/vita/tools/job_vita_smoke.sh      (one heavy.sh turn, a minute)
cd "$(dirname "$0")/../../.."
export VITASDK=/opt/vitasdk PATH=/opt/vitasdk/bin:$PATH
BR=${BUILD_ROOT:-/home/claude/msc/build/np/NPV3/c1}
/home/claude/msc/tools/heavy.sh -t 300 make -f platform/vita/Makefile.vpk smoke NP_VBUILD=$BR/vita/pkg ${NP_PLAYER:+NP_PLAYER=$NP_PLAYER} 2>&1 | grep -v "^/opt/vitasdk.*warning\|wchar_t yet\|32-bit enums\|GNU-stack\|NOTE: This" | tail -15
ls -la $BR/vita/pkg/np/np-smoke.* 2>&1 | tail -4
