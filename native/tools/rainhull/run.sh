#!/bin/sh
# builds and runs the rainhull host test (RAINHULL_BREAK=1 in the environment: the live mutant, must exit non-zero)
set -e
R=$(cd "$(dirname "$0")/../../.." && pwd)
T=${TMPDIR:-/tmp}/rainhull_test.$$
${CXX:-g++} -std=gnu++11 -O1 -Wall -I"$R/native/include" "$R/native/tools/rainhull/rainhull_test.cpp" -o "$T"
"$T"; rc=$?; rm -f "$T"; exit $rc
