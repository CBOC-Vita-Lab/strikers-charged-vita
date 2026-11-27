#!/bin/bash
# Host test of np_launch_guard.h (the launch marker, stab13): the plain test must pass, and each mutant of the header's rules must be caught.
set -o pipefail
H=$(cd "$(dirname "$0")" && pwd); O=${1:-/tmp/np_launch_guard_test}; mkdir -p "$O"
F="-std=gnu++17 -O1 -g -Wall -Wextra"
g++ $F -I"$H/../src" "$H/test_launch_guard.cpp" -o "$O/t_plain" && "$O/t_plain" || exit 1
mut() {   # name sed-expression
    sed "$2" "$H/../src/np_launch_guard.h" > "$O/np_launch_guard.h"
    if cmp -s "$O/np_launch_guard.h" "$H/../src/np_launch_guard.h"; then echo "mutant $1: the sed did not change the header"; exit 1; fi
    g++ $F -I"$O" "$H/test_launch_guard.cpp" -o "$O/t_mut" 2>/dev/null || { echo "mutant $1: does not compile"; exit 1; }
    if "$O/t_mut" >/dev/null 2>&1; then echo "mutant $1: NOT CAUGHT"; exit 1; fi
    echo "mutant $1: caught"
}
mut "clean history is safe"   's/if (previous <= 0) return State::kNormal;/if (previous < 0) return State::kNormal;/'
mut "never gives up"          's/return previous <= kSafeMax ? State::kSafe : State::kGaveUp;/return State::kSafe;/'
mut "never safe"              's/return previous <= kSafeMax ? State::kSafe : State::kGaveUp;/return State::kGaveUp;/'
mut "overrides the owner"     's/if (!env.IsPlanDefault(r.name)) continue;//'
mut "safe on every state"     's/if (state != State::kSafe) return 0;//'
mut "stable without time"     's/ \&\& elapsedMs >= static_cast<unsigned long long>(minutes) \* 60000ull//'
mut "stable without frames"   's/return frames >= kStableFrames \&\& /return /'
mut "marker not counted up"   's/return previous >= kCountCap ? kCountCap : previous + 1;/return previous;/'
echo "run_launch_guard_test: all ok"
