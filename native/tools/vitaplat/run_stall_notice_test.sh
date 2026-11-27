#!/bin/bash
# The stall notice (vita_notice.h): the words by quiet time and the banner drawn into a frame buffer. No game data, no device; a second.
#   tools/vitaplat/run_stall_notice_test.sh
set -e
root=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$root"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
vita=native/platform/vita
"${CXX:-c++}" -std=c++17 -O1 -Wall -Wextra -I$vita/src $vita/tests/test_notice.cpp -o "$tmp/test_notice"
"$tmp/test_notice"
