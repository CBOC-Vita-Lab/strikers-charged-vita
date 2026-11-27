#!/bin/bash
# Mutation check of np_bind_chain.h: each mutant breaks one rule in a copy of the header and bind_chain_test MUST then fail.
cd "$(dirname "$0")/../.."
OUT=${1:-/tmp/bindchain-mut}; mkdir -p "$OUT"; bad=0
clang++ -std=gnu++11 -I. tools/net/bind_chain_test.cpp -o "$OUT/t" && "$OUT/t" || exit 1
run() {
  python3 - "$2" "$3" "$OUT/np_bind_chain.h" <<'PY' || { echo "mutant $1: pattern not found"; bad=1; return; }
import sys
old, new, out = sys.argv[1:4]
s = open('platform/sdk/net/np_bind_chain.h').read()
if old not in s: sys.exit(1)
open(out, 'w').write(s.replace(old, new, 1))
PY
  clang++ -std=gnu++11 -I. -DBIND_CHAIN_HEADER="\"$OUT/np_bind_chain.h\"" tools/net/bind_chain_test.cpp -o "$OUT/tm" 2>"$OUT/err" || { head "$OUT/err"; bad=1; return; }
  if "$OUT/tm" >"$OUT/run.txt" 2>&1; then echo "mutant $1: SURVIVED"; bad=1; else echo "mutant $1: caught ($(grep -c '^FAIL' "$OUT/run.txt") failed checks)"; fi
}
run lan-default-high 'kLanDefaultPort = 25100;' 'kLanDefaultPort = 41100;'
run online-default-high 'kOnlineDefaultPort = 25110;' 'kOnlineDefaultPort = 41000;'
run lan-in-galaxy-range 'kLanDefaultPort = 25100;' 'kLanDefaultPort = 24100;'
run chains-overlap 'kLanPortTries = 4;' 'kLanPortTries = 12;'
run no-system-fallback 'if (anySystemPort) {' 'if (false) {'
run always-system-fallback 'if (first == 0) tries = 0, anySystemPort = true;' 'anySystemPort = true;'
run no-next-port 'for (int t = 0; t < tries && unsigned' 'for (int t = 0; t < 1 && unsigned'
run wraps-past-65535 '<= 65535u' '<= 70000u'
run stops-on-first-error 'if (onFail) onFail(ctx, port, rc);
    }
    if (anySystemPort)' 'if (onFail) onFail(ctx, port, rc);
        break;
    }
    if (anySystemPort)'
run off-by-one-range 'for (int t = 0; t < tries &&' 'for (int t = 0; t < tries - 1 &&'
run lost-first-error 'if (!r.firstError) r.firstError = rc;
        if (onFail) onFail(ctx, port, rc);' 'if (onFail) onFail(ctx, port, rc);'
run port0-fail-ignored 'if (rc >= 0) { r.ok = true; r.requested = 0; return r; }' 'r.ok = true; return r;'
run no-failure-log 'if (onFail) onFail(ctx, port, rc);
    }
    if (anySystemPort)' '}
    if (anySystemPort)'
[ $bad = 0 ] && echo "all mutants caught"
exit $bad
