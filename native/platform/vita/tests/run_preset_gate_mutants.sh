#!/bin/bash
# The preset gate test (test_settings.cpp, np_picture_presets.h kGates) must fail for each mutant below, and pass unmutated. Usage: run_preset_gate_mutants.sh [outdir]
set -eu
H=$(cd "$(dirname "$0")" && pwd); S="$H/../src"; O=${1:-/tmp/np_gate_mutants}; rm -rf "$O"; mkdir -p "$O"
build() {   # dir of mutated copies of src -> binary
    g++ -std=c++17 -I"$1" -I"$S" -I"$H" "$H/test_settings.cpp" "$1/np_settings.cpp" "$S/vita_plan.cpp" -o "$2" 2>&1 | grep -E "error" || true
}
cp -r "$S" "$O/src0"; build "$O/src0" "$O/ok"
if "$O/ok" > "$O/ok.out" 2>&1; then echo "unmutated: pass ($(tail -1 "$O/ok.out"))"; else echo "UNMUTATED FAILS"; tail -3 "$O/ok.out"; exit 1; fi
bad=0; n=0
mutant() {   # name file sed-expr
    n=$((n+1)); local d="$O/m$n"; cp -r "$S" "$d"
    sed -i -e "$3" "$d/$2"
    if cmp -s "$S/$2" "$d/$2"; then echo "mutant $1: nothing changed"; bad=1; return; fi
    build "$d" "$d/t"
    if "$d/t" > "$d/out" 2>&1; then echo "MUTANT SURVIVED: $1"; bad=1; else echo "mutant killed: $1 ($(grep -m1 '^preset gate' "$d/out" || grep -m1 FAIL "$d/out"))"; fi
}
# columns that break a gate
mutant "MSC_PICTURE=lock60 with a size"   np_picture_presets.h 's/    {"MSC_LOCK60", "1"},/    {"MSC_LOCK60", "1"},\n    {"MSC_VITA_RENDER", "640x362"},/'
# the checker
mutant "gate never broken"                np_picture_presets.h 's/if ((g.rule == kNeedsOn) != on) return &g;/if (false) return \&g;/'
mutant "NeedsOff ignored"                 np_picture_presets.h 's/if ((g.rule == kNeedsOn) != on) return &g;/if (g.rule == kNeedsOn \&\& !on) return \&g;/'
mutant "unset gate counts as on"          np_picture_presets.h 's/bool on = gv ? KeyOn(g.gate, gv) : g.gateDefaultOn;/bool on = gv ? KeyOn(g.gate, gv) : true;/'
mutant "child at 0 counts as set"         np_picture_presets.h 's/if (!child || !KeyOn(g.child, child)) continue;/if (!child) continue;/'
mutant "off is not off"                   np_picture_presets.h 's/!std::strcmp(value, "off") || //'
exit $bad
