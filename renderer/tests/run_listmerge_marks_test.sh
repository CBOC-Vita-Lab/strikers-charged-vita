#!/bin/bash
# nxboot25 R2-11: the shader list parser and merger with record type 5 (marks), with mutants, and byte-for-byte against tools/seed/shl.py merge.
#   tests/run_listmerge_marks_test.sh [outdir]     (a few seconds, no game data)
set -eu
V=$(cd "$(dirname "$0")/.." && pwd); O=${1:-/tmp/vgx_listmerge_marks}; mkdir -p "$O/m"
SHL=${SHL:-$V/../native/tools/seed/shl.py}
CXX=${CXX:-g++}
F="-std=c++17 -O1 -g -Wall -Wextra"
$CXX $F -I"$V/vita" "$V/tests/listmerge_marks.cpp" -o "$O/t"
"$O/t" "$O"
if [[ -f $SHL ]]; then
    python3 -I "$SHL" merge -o "$O/ref.shl" "$O/a.shl" "$O/b.shl" > /dev/null
    cmp "$O/ref.shl" "$O/merged.shl" && echo "  the merge equals shl.py merge byte for byte"
else echo "  (no shl.py at $SHL: the byte comparison with the Python reference is skipped)"; fi
mut() {   # name, sed expression on the header
    local d="$O/m/$1"; mkdir -p "$d"; sed "$2" "$V/vita/vgx_listmerge.h" > "$d/vgx_listmerge.h"
    cmp -s "$d/vgx_listmerge.h" "$V/vita/vgx_listmerge.h" && { echo "mutant $1 did not change the header"; exit 1; }
    $CXX $F -I"$d" "$V/tests/listmerge_marks.cpp" -o "$d/t"
    if "$d/t" > "$d/out" 2>&1; then echo "MUTANT $1 SURVIVED"; exit 1; fi
    echo "  mutant $1 killed ($(grep -c FAIL "$d/out") checks)"
}
mut refuse_type5     's/t == 5 \&\& allowMarks/t == 55 \&\& allowMarks/'          # the bug: record 5 falls to "return L"
mut no_mark_bounds   '/t == 5/,/L.marks.emplace_back/{/v >= L.vs.size/d}'
mut drop_marks_out   's/    for (const auto& pr : prog) if (markSeen.count/    for (const auto\& pr : prog) if (false \&\& markSeen.count/'
mut no_remap         's/markSeen.emplace((static_cast<uint64_t>(mapV\[mk.first\]) << 32) | mapF\[mk.second\], true)/markSeen.emplace((static_cast<uint64_t>(mk.first) << 32) | mk.second, true)/'
mut marks_first_only 's/        for (const auto\& mk : L->marks)/        if (w == 0) for (const auto\& mk : L->marks)/'
mut count_marks      's/markSeen.emplace((static_cast<uint64_t>(mapV\[mk.first\]) << 32) | mapF\[mk.second\], true);/if (markSeen.emplace((static_cast<uint64_t>(mapV[mk.first]) << 32) | mapF[mk.second], true).second) fromSecond += (w == 1);/'
echo "run_listmerge_marks_test: ok"
