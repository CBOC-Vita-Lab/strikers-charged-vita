#!/bin/bash
# build dt_a (x86-64): the recompiler's translation of the retail functions in fnlist.txt over the synthetic game state
set -e
cd "$(dirname "$0")"
[ -f index.pkl ] || ./mkindex.py
./gen_a.py gen_fns2.inc $(cat fnlist.txt)
g++ -std=c++20 -O1 -w -I shim -I/home/claude/msc/one/port/runtime/include dt_a.cpp -o ${BIN:-dt_a}
