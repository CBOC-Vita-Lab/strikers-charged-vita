#!/bin/bash
# The console audio output thread (np_vita_audio.cpp) on fakes of the Vita calls: three rotating output buffers, none rewritten while it plays.
# No game data, no device; a few seconds.   tools/audio/run_audio_out_test.sh [outdir]
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-${BUILD:-/tmp/np_audio_out}}
mkdir -p "$OUT"
make -s -C platform/vita OUT="$OUT" aocheck
