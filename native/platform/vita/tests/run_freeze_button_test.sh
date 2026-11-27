#!/bin/bash
# Host test of the freeze button's rule plus mutants of the header (each must make the test fail). Usage: run_freeze_button_test.sh [outdir]
set -eu
H=$(cd "$(dirname "$0")" && pwd); O=${1:-/tmp/np_freeze_test}; rm -rf "$O"; mkdir -p "$O/src"
build() { g++ -std=gnu++17 -O1 -Wall -Wextra -fsanitize=address,undefined -I"$1" "$H/test_freeze_button.cpp" -o "$2"; }
build "$H/../src" "$O/t_ok"; "$O/t_ok"
H1="$H/../src/np_freeze_button.h"; bad=0; n=0
mutant() {   # name sed-expression
    n=$((n+1)); local d="$O/m$n"; mkdir -p "$d"; sed -e "$2" "$H1" > "$d/np_freeze_button.h"
    if cmp -s "$H1" "$d/np_freeze_button.h"; then echo "mutant $1: sed changed nothing"; bad=1; return; fi
    build "$d" "$d/t"
    if "$d/t" > "$d/out" 2>&1; then echo "MUTANT SURVIVED: $1"; bad=1; else echo "mutant killed: $1 ($(tail -1 "$d/out"))"; fi
}
mutant "hold 2.9 s"            's/kHoldMs = 3000/kHoldMs = 2900/'
mutant "hold 3.1 s"            's/kHoldMs = 3000/kHoldMs = 3100/'
mutant "no 20 s ignore"        's/ || nowMs < kIgnoreMs//'
mutant "ignore only 10 s"      's/kIgnoreMs = 20000/kIgnoreMs = 10000/'
mutant "ignore 25 s"           's/kIgnoreMs = 20000/kIgnoreMs = 25000/'
mutant "release does not reset" 's/if (!allDown || blocked || nowMs < kIgnoreMs) {/if (!allDown) return false; if (blocked || nowMs < kIgnoreMs) {/'
mutant "settings screen does not block" 's/!allDown || blocked ||/!allDown ||/'
mutant "fires every poll"      's/            fired = true;/            fired = false;/'
mutant "unset means off in player build" 's/return playerBuild;/return false;/'
mutant "0 means on"            "s/!(env\[0\] == '0' \&\& env\[1\] == '\\\\0')/true/"
mutant "off-by-one: > not >="  's/nowMs - holdStartMs >= kHoldMs/nowMs - holdStartMs > kHoldMs/'
exit $bad
