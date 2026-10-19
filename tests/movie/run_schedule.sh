#!/usr/bin/env bash
set -euo pipefail
tests="$(cd "$(dirname "$0")" && pwd)"
root="${SOURCE_ROOT:-$(cd "$tests/../.." && pwd)}"
out="${OUTPUT_ROOT:-$root/build/movie-tests}"
cd "$root/native"
mkdir -p "$out/tests"
clang++ --target=arm-linux-gnueabihf -std=gnu++11 -O1 -w -fno-strict-return \
  -DNP_NATIVE=1 -DNP_DISC_HOST_ORDER=1 -DNP_TARGET_ARMHF=1 \
  -DBUILD_VERSION=0 -DVERSION_R4QE01 -DNDEBUG=1 -DdNODEBUG=1 -DdSINGLE=1 \
  -fno-strict-aliasing -fwrapv -fno-short-enums -fshort-wchar -funsigned-char \
  -ffunction-sections -fdata-sections -include np/compat.h \
  -Iplatform/gx/include -I. -Iinclude -Ilibs/RVL_SDK/include -Iref/RVL_SDK \
  -Ilibs/Runtime/include -idirafter libs/MSL_C/include -idirafter libs/MetroTRK/include \
  "$tests/movie_schedule.cpp" -Wl,--gc-sections -o "$out/tests/movie_schedule"
failed=0
for test in backpressure read_backpressure task_starvation late_frame last_audio_slot capacity_race preview steady not_due end; do
  MSC_TEST_CASE="$test" qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf "$out/tests/movie_schedule" || failed=1
done
MSC_TEST_CASE=legacy MSC_MOVIE_CATCHUP=0 qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf "$out/tests/movie_schedule" || failed=1
exit "$failed"
