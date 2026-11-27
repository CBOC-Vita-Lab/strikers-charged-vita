#!/bin/bash
# Short movies of the disc through the game's movie player: pictures and audio samples against what the THP file authors.
# Needs the disc dump (skipped without it) and the SDK test build:   tools/thp/run_movies_test.sh [sdk build dir]
set -e
cd "$(dirname "$0")/../.."
B=${1:-/work/msc/build/np/NPS/host32}
make -s -C platform/sdk T=host32 B="$B" "$B/tests/test_thp_movies"
for cfg in "MSC_THP_HWJPEG=0 MSC_THP_NOWAIT=0" "MSC_THP_HWJPEG=fake MSC_THP_NOWAIT=1"; do
  echo "== $cfg"
  env $cfg "$B/tests/test_thp_movies" stad_crystalcanyon.thp nlgintro_PAL.thp 2>&1 | grep -v '^\['
done
