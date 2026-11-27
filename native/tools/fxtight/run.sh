#!/bin/sh
# Host test of np/fxtight.h. Optional: a folder of texture dumps (fxtight_dump.py) adds the whole disc.
set -e
d=$(dirname "$0"); o=${TMPDIR:-/tmp}/fxtight_test.$$
g++ -std=c++17 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra -I"$d/../../include" "$d/fxtight_test.cpp" -o "$o"
"$o" "$@"; r=$?; rm -f "$o"; exit $r
