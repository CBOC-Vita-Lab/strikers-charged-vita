#!/bin/bash
# Mutation check of np_quick_policy.h: each mutant breaks one rule in a copy of the header and quick_policy_test MUST then fail.
cd "$(dirname "$0")/../.."
OUT=${1:-/tmp/quickpolicy-mut}; mkdir -p "$OUT"; bad=0
clang++ -std=gnu++11 -I. tools/net/quick_policy_test.cpp -o "$OUT/t" && "$OUT/t" || exit 1
run() {
  python3 - "$2" "$3" "$OUT/np_quick_policy.h" <<'PY' || { echo "mutant $1: pattern not found"; bad=1; return; }
import sys
old, new, out = sys.argv[1:4]
s = open('platform/sdk/dwc/np_quick_policy.h').read()
if old not in s: sys.exit(1)
open(out, 'w').write(s.replace(old, new, 1))
PY
  clang++ -std=gnu++11 -I. -DQUICK_POLICY_HEADER="\"$OUT/np_quick_policy.h\"" tools/net/quick_policy_test.cpp -o "$OUT/tm" 2>"$OUT/err" || { head "$OUT/err"; bad=1; return; }
  if "$OUT/tm" >"$OUT/run.txt" 2>&1; then echo "mutant $1: SURVIVED"; bad=1; else echo "mutant $1: caught ($(grep -c '^FAIL' "$OUT/run.txt") failed checks)"; fi
}
run never-quick '        s.quick = true;' '        s.quick = false;'
run no-jump 'return true;' 'return false;'
run jump-every-hub '!s.done && wanted' 'wanted'
run hub-keeps-quick 'scene == kQsHub || scene == kQsLogin ||' 'scene == kQsLogin ||'
run login-keeps-quick 'scene == kQsLogin || scene == kQsMainMenu) s.quick' 'scene == kQsMainMenu) s.quick'
run menu-keeps-quick 'scene == kQsLogin || scene == kQsMainMenu) s.quick' 'scene == kQsLogin) s.quick'
run login-no-rearm 'if (scene == kQsLogin) s.done = false;' ''
run ignores-wanted 'scene == kQsHub && !s.done && wanted' 'scene == kQsHub && !s.done'
exit $bad
