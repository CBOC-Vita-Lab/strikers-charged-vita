#!/bin/bash
# Mutation check of np_delay19.h: each mutant breaks one rule in a copy of the header and delay19_test MUST then fail.
cd "$(dirname "$0")/../.."
OUT=${1:-/tmp/delay19-mut}; mkdir -p "$OUT"; bad=0
clang++ -std=gnu++11 -I. tools/net/delay19_test.cpp -o "$OUT/t" && "$OUT/t" || exit 1
run() {
  python3 - "$2" "$3" "$OUT/np_delay19.h" <<'PY' || { echo "mutant $1: pattern not found"; bad=1; return; }
import sys
old, new, out = sys.argv[1:4]
s = open('platform/sdk/net/np_delay19.h').read()
if old not in s: sys.exit(1)
open(out, 'w').write(s.replace(old, new, 1))
PY
  clang++ -std=gnu++11 -I. -DDELAY19_HEADER="\"$OUT/np_delay19.h\"" tools/net/delay19_test.cpp -o "$OUT/tm" 2>"$OUT/err" || { head "$OUT/err"; bad=1; return; }
  if "$OUT/tm" >"$OUT/run.txt" 2>&1; then echo "mutant $1: SURVIVED"; bad=1; else echo "mutant $1: caught ($(grep -c '^FAIL' "$OUT/run.txt") failed checks)"; fi
}
run floor-global 'return agreed >= 1 ? float(agreed) : gameMin; }' 'return gameMin; }'
run start-game 'return agreed >= 1 ? float(agreed) : gameDefault; }' 'return gameDefault; }'
run max-cut 'agreed >= 1 && float(agreed) > gameMax ? float(agreed) : gameMax' 'gameMax'
run decay-no-floor 'return v < floorV ? floorV : v;' 'return v;'
run ceiling-off 'return v > maxV ? maxV : v;' 'return v;'
run inc-wrong 'const float v = mult + inc;' 'const float v = mult + dec;'
run drain-default-on 'return v && v[0] && !(v[0] == '"'"'0'"'"' && v[1] == 0); }' 'return !(v && v[0] == '"'"'0'"'"' && v[1] == 0); }'
run retry-uncapped 'n > 8000 ? 8000 : n' 'n'
[ $bad = 0 ] && echo "all mutants caught"
exit $bad
