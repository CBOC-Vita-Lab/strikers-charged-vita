#!/bin/bash
# Host test of vgx_copyguard.h with mutants (each must fail). Usage: run_copy_guard_test.sh [outdir]
set -eu
H=$(cd "$(dirname "$0")" && pwd); O=${1:-/tmp/np_copyguard_test}; rm -rf "$O"; mkdir -p "$O"
build() { g++ -std=gnu++17 -O1 -fsanitize=address,undefined -I"$1" -I"$H/../src" "$H/copy_guard.cpp" -o "$2"; }
cp "$H/../src/vgx_copyguard.h" "$O/orig.h"
build "$H/../src" "$O/ok"; "$O/ok" 2>&1 | tail -3
bad=0; n=0
mutant() {
    n=$((n+1)); local d="$O/m$n"; mkdir -p "$d"; sed -e "$2" "$H/../src/vgx_copyguard.h" > "$d/vgx_copyguard.h"
    if cmp -s "$H/../src/vgx_copyguard.h" "$d/vgx_copyguard.h"; then echo "mutant $1: nothing changed"; bad=1; return; fi
    build "$d" "$d/t" 2>/dev/null
    if "$d/t" > "$d/out" 2>&1; then echo "MUTANT SURVIVED: $1"; bad=1; else echo "mutant killed: $1"; fi
}
mutant "no bounds check"      's/arg < buf.copyTex.size()/true/'
mutant "off by one (<=)"      's/arg < buf.copyTex.size()/arg <= buf.copyTex.size()/'
mutant "dead record not skip" 's/c.skip = true;/c.skip = false;/'
mutant "miss not counted"     's/Count().fetch_add(1, std::memory_order_relaxed) + 1/Count().load()+1/'
exit $bad
