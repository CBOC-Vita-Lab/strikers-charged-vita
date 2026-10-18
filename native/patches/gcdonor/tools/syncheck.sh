#!/bin/bash
# Syntax-check the files touched by patches/gcdonor on top of NPB's current tree.
#   tools/syncheck.sh [target=host32|armhf|vita] [file ...]    (default: every file in series that is C/C++)
# Copies /home/claude/msc/np (working tree, NPB's WIP included) to a scratch dir, applies the series with
# `git apply` (non-fatal per patch, reported), then compiles each touched source with NPB's own flags
# from its Makefile (make ... syntax SUBSET=<file>). Output only under the scratch dir.
set -e
TARGET=${1:-host32}; shift || true
S=/tmp/claude-0/-home-claude/2e068ce2-50b7-5168-bd6a-083a34fe1600/scratchpad/npd/syn
OUT=/home/claude/msc/npw/NPD/patches/gcdonor
rm -rf "$S"; mkdir -p "$S"
cd /home/claude/msc/np
cp -r src include libs ref Makefile "$S"/ 2>/dev/null || cp -r src include Makefile "$S"/
[ -d platform ] && cp -r platform "$S"/ || true
cd "$S"
while read p; do
  if git apply --whitespace=nowarn "$OUT/$p" 2>/tmp/claude-0/ap.err; then :; else echo "APPLY FAIL $p: $(head -1 /tmp/claude-0/ap.err)"; fi
done < "$OUT/series"
files="$@"
[ -z "$files" ] && files=$(sed 's/\.patch$//' "$OUT/series" | grep -E '\.(c|cpp)$' || true)
for f in $files; do
  rm -rf "$S/b"
  if make -s TARGET=$TARGET BUILD_ROOT="$S/b" SUBSET="$f" syntax >"$S/make.log" 2>&1; then echo "ok   $f"; else echo "FAIL $f"; head -5 "$S/make.log"; fi
done
