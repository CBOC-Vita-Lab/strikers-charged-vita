#!/bin/bash
# Builds and runs the tests of platform/sdk/net on host32 (clang -m32, the np build's flags). No game data; a few seconds.
#   tools/net/run_tests.sh [outdir]        default /home/claude/msc/build/npm/net
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-/home/claude/msc/build/npm/net}
mkdir -p "$OUT"
FL="-m32 -isystem /usr/include/x86_64-linux-gnu -DNP_TARGET_HOST32=1 -DNP_NATIVE=1 -DBUILD_VERSION=0 -DVERSION_R4QE01 -DNDEBUG=1 \
 -I. -Iinclude -Isrc/ode -Ilibs/RVL_SDK/include -Ilibs/Runtime/include -idirafter libs/MSL_C/include -idirafter libs/MetroTRK/include \
 -fno-strict-aliasing -fwrapv -fno-short-enums -fshort-wchar -fno-delete-null-pointer-checks -w -include np/compat.h \
 -std=gnu++11 -fno-exceptions -fno-rtti -fdeclspec -O1 -g1"
clang++ $FL platform/sdk/net/vlink.cpp platform/sdk/net/vlink_udp.cpp platform/sdk/net/so_net.cpp tools/net/net_test.cpp \
  platform/compat/np_compat.c platform/compat/np_gamefixes.cpp -x c++ -o "$OUT/net_test" -Wl,-z,muldefs -lpthread 2>"$OUT/build.err" || { head -30 "$OUT/build.err"; exit 1; }
"$OUT/net_test"
