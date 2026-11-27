#!/bin/bash
# Mutation check of the keep-awake filler in vlink.cpp: each mutant breaks one rule in a copy of vlink.cpp and wake14_test MUST then fail.
cd "$(dirname "$0")/../.."
OUT=${1:-/tmp/wake14-mut}; mkdir -p "$OUT"; bad=0
clang++ -std=gnu++11 -I. platform/sdk/net/vlink.cpp tools/net/wake14_test.cpp -o "$OUT/t" 2>"$OUT/err" || { head "$OUT/err"; exit 1; }
"$OUT/t" >"$OUT/base.txt" 2>&1 || { grep '^FAIL' "$OUT/base.txt"; exit 1; }
tail -1 "$OUT/base.txt"
run() {
  python3 - "$2" "$3" "$OUT/vlink.cpp" <<'PY' || { echo "mutant $1: pattern not found"; bad=1; return; }
import sys
old, new, out = sys.argv[1:4]
s = open('platform/sdk/net/vlink.cpp').read()
if old not in s: sys.exit(1)
open(out, 'w').write(s.replace(old, new, 1))
PY
  clang++ -std=gnu++11 -I. "$OUT/vlink.cpp" tools/net/wake14_test.cpp -o "$OUT/tm" 2>"$OUT/err" || { head "$OUT/err"; bad=1; return; }
  if "$OUT/tm" >"$OUT/run.txt" 2>&1; then echo "mutant $1: SURVIVED"; bad=1; else echo "mutant $1: caught ($(grep -c '^FAIL' "$OUT/run.txt") failed checks)"; fi
}
run filler-reaches-the-game '        if (filler) {   // wave 14' '        if (0) {   // wave 14'
run no-rate-limit 'if (e.lastTxMs && now + 1 - e.lastTxMs < keepAwakeMs) continue;' ''
run no-peer-window 'if (!e.used || !e.lastUnicastMs || now + 1 - e.lastUnicastMs >= kKeepAwakePeerWindowMs) continue;' 'if (!e.used) continue;'
run broadcast-makes-a-peer 'NoteTx(addrs[i], false, now);' 'NoteTx(addrs[i], true, now);'
run real-send-not-noted '    if (m->Tracking()) m->NoteTxTo(dstVip, broadcast);
    return int(len);' '    return int(len);'
run suspended-ignored 'if (!keepAwakeMs || keepAwakeSuspended || !up' 'if (!keepAwakeMs || !up'
run lost-ignored ' || __atomic_load_n(&lost, __ATOMIC_ACQUIRE) || (backend' ' || (backend'
run adhoc-gets-filler ' || (backend && backend->name && strcmp(backend->name, "adhoc") == 0)) return;' ') return;'
run filler-renews-the-window 'NoteTx(e, false, now);   // (also on a refusal' 'NoteTx(e, true, now);   // (also on a refusal'
run filler-sent-when-off 'if (!keepAwakeMs || keepAwakeSuspended' 'if (keepAwakeSuspended'
run filler-with-payload-flag 'Put16(frame + 22, 0);
            const PeerAddr to' 'Put16(frame + 22, 1000);
            const PeerAddr to'
[ $bad = 0 ] && echo "all mutants caught"
exit $bad
