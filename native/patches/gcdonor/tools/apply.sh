#!/bin/bash
# Apply the gcdonor patches to the np tree in the current directory (or $1), in series order.
#   patches/gcdonor/tools/apply.sh [np-root]       add --check as $2 to only test
# Each patch is independent (one path per patch) except where docs/gcdonor.md lists a dependency;
# a patch that no longer applies cleanly is retried with --3way and reported, never forced.
set -u
ROOT=${1:-.}; MODE=${2:-}
OUT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2
fail=0
while read p; do
  [ -z "$p" ] && continue
  if [ "$MODE" = "--check" ]; then
    git apply --check "$OUT/$p" 2>/dev/null && echo "ok     $p" || { echo "FAIL   $p"; fail=1; }
  else
    if git apply --whitespace=nowarn "$OUT/$p" 2>/dev/null; then echo "applied $p";
    elif git apply --3way --whitespace=nowarn "$OUT/$p" 2>/dev/null; then echo "3way   $p (check conflicts)";
    else echo "FAIL   $p"; fail=1; fi
  fi
done < "$OUT/series"
exit $fail
