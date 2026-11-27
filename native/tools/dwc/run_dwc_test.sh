#!/bin/bash
# Builds and runs the tests of the online layer (platform/sdk/dwc) on host32 (clang -m32, the np build's flags). No game data; seconds.
#   tools/dwc/run_dwc_test.sh [outdir]
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-${TMPDIR:-/tmp}/msc_dwc_test}
mkdir -p "$OUT"
FL="-m32 -isystem /usr/include/x86_64-linux-gnu -DNP_TARGET_HOST32=1 -DNP_NATIVE=1 -DBUILD_VERSION=0 -DVERSION_R4QE01 -DNDEBUG=1 \
 -I. -Iinclude -Isrc/ode -Ilibs/RVL_SDK/include -Ilibs/Runtime/include -idirafter libs/MSL_C/include -idirafter libs/MetroTRK/include \
 -fno-strict-aliasing -fwrapv -fno-short-enums -fshort-wchar -fno-delete-null-pointer-checks -w -include np/compat.h \
 -std=gnu++11 -fno-exceptions -fno-rtti -fdeclspec -O1 -g1"
D=platform/sdk/dwc
clang++ $FL $D/np_online_api.cpp $D/np_online_fake.cpp $D/np_online_queue.cpp $D/np_online_json.cpp $D/np_online_http.cpp platform/compat/np_gamefixes.cpp $D/np_online_store.cpp $D/np_online_name.cpp $D/np_dwc_account.cpp \
  $D/np_dwc_error.cpp $D/np_dwc_login.cpp $D/np_dwc_friend.cpp $D/np_dwc_rank.cpp platform/sdk/net/np_mpdiag.cpp tools/dwc/dwc_test.cpp -o "$OUT/dwc_test" -lpthread 2>"$OUT/build.err" || { head -60 "$OUT/build.err"; exit 1; }
"$OUT/dwc_test"
# the held friends poll hands its hold to the HTTPS layer as the no-data limit (an ordinary request does not)
clang++ $FL $D/np_online_api.cpp $D/np_online_fake.cpp $D/np_online_queue.cpp $D/np_online_json.cpp $D/np_online_http.cpp platform/compat/np_gamefixes.cpp $D/np_online_store.cpp $D/np_online_name.cpp $D/np_dwc_account.cpp \
  $D/np_dwc_error.cpp $D/np_dwc_login.cpp $D/np_dwc_friend.cpp $D/np_dwc_rank.cpp tools/dwc/held_io_test.cpp -o "$OUT/held_io_test" -lpthread 2>>"$OUT/build.err" || { head -60 "$OUT/build.err"; exit 1; }
"$OUT/held_io_test"
