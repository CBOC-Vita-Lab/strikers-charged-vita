#!/bin/bash
# run_host_tests.sh [-j N] [-t SECONDS] [-f REGEX] [--list] [--quick] [--pytest] [--out DIR]    (from Galaxy's tools/run_host_tests.sh and run_all_host_tests.sh, 22 Nov 2026)
# ONE command for the host test scripts of the repo that need no game data, console, emulator or game build:
#   renderer/tests/run_*_test.sh   native/**/run_*test*.sh   native/**/test_*.sh   (every script runs its own compile and its own deliberately broken variant)
# and, with --pytest, `python3 -m pytest -q tests` as one more row.
# Each script runs under nice with its OWN temp folder (TMPDIR and BUILD point at <out>/tmp/<name>: tests that build into /tmp/<fixed name> cannot collide with -j 2).
# One line per test and a table at the end:
#   PASS     exit 0            FAIL    built and a check failed            NOBUILD  the compiler or linker failed (never counted as a pass)
#   TIMEOUT  over -t seconds (default 900)                               SKIP     exit 77, or the last line starts "SKIP", or the name is in tools/host_tests_skip.txt (needs an emulator, a Vita, arguments, a server)
# Exit 0 only when nothing failed, did not build or timed out. A SKIP is printed, never counted as a pass.
# Sanitizers: tools/hosttest_shim (first on PATH) drops -fsanitize=X when the compiler cannot link that runtime (clang without its ASan runtime: the renderer tests used to need CXX=g++);
#   HOSTTEST_SAN=on keeps every flag (a missing runtime is then a NOBUILD). CXX is left as the scripts default it (set CXX=g++ to force one).
# --quick: only the scripts named in tools/host_tests_quick.txt (one name a line; --write-quick rewrites it from this run: PASS rows of at most 8 s, until 420 s)
# Results: <out>/results.tsv (status, name, seconds, last line), <out>/<name>.out for every test that did not pass. Default out: /tmp/host-tests-<time>.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"; cd "$root" || exit 2
TMO=900; JOBS=2; FILT=; LIST=0; QUICK=0; PYT=0; OUT=; WRITEQ=0
while [ $# -gt 0 ]; do case "$1" in -t) TMO=$2; shift;; -f) FILT=$2; shift;; -j) JOBS=$2; shift;; --list) LIST=1;; --quick) QUICK=1;; --pytest) PYT=1;; --out) OUT=$2; shift;; --write-quick) WRITEQ=1;;
  -h|--help) sed -n 2,17p "$0"; exit 0;; *) echo "usage: $0 [-j N] [-t SECONDS] [-f REGEX] [--list] [--quick] [--pytest] [--out DIR] [--write-quick]"; exit 2;; esac; shift; done
scripts=$( { ls renderer/tests/run_*_test.sh renderer/tests/run_*tests.sh; find native -name 'run_*test*.sh' -o -name 'test_*.sh' -o -name 'run_all.sh' -o -name 'run_tests.sh'; } 2>/dev/null | sort -u | grep -E -- "${FILT:-.}")
if [ $QUICK = 1 ]; then q=$(sed 's/#.*//' tools/host_tests_quick.txt 2>/dev/null | awk 'NF{print $1}'); scripts=$(for s in $scripts; do n=$(basename "$s" .sh); printf '%s\n' "$q" | grep -qxF "$n" && echo "$s"; done); fi
[ $LIST = 1 ] && { echo "$scripts"; [ $PYT = 1 ] && echo "pytest tests"; exit 0; }
OUT=${OUT:-/tmp/host-tests-$(date +%m%d-%H%M%S)}; mkdir -p "$OUT/tmp"; RES=$OUT/results.tsv; : > "$RES"
export NP_TEST_SLACK=${NP_TEST_SLACK:-4}   # (timing bounds in a few host tests, e.g. native/tools/net/https_test.cpp: this runner is usually busy)
export HOSTTEST_REAL_PATH=$PATH HOSTTEST_CACHE=${HOSTTEST_CACHE:-/tmp/hosttest-san-cache-$$}
export PATH="$root/tools/hosttest_shim:$PATH"
# The scripts default MSC=/work/msc (deps/xxHash, build dir of the old build box): give them a private MSC whose deps/xxHash is the repo's own copy.
if [ -z "${HOSTTEST_KEEP_MSC:-}" ]; then mkdir -p "$OUT/msc/deps"; ln -sfn "$root/renderer/third_party" "$OUT/msc/deps/xxHash"; export MSC="$OUT/msc"; fi
skiplist=$(sed 's/#.*//' tools/host_tests_skip.txt 2>/dev/null | awk 'NF{print $1}')
run_one() {
  s=$1; name=$(basename "$s" .sh); case " $dupnames " in *" $name "*) name=$(basename "$(dirname "$s")")__$name;; esac   # (run_tests.sh exists in four folders: tools__net__run_tests style names keep their .out files apart)
   [ "$s" = PYTEST ] && name=pytest-tests; t0=$(date +%s); o="$OUT/$name.out"; td="$OUT/tmp/$name"; mkdir -p "$td"
  if printf '%s\n' "$skiplist" | grep -qxF "$name"; then st=SKIP; echo "SKIP: listed in tools/host_tests_skip.txt" >"$o"; rc=0
  else
    if [ "$s" = PYTEST ]; then nice -n "${HOSTTEST_NICE:-15}" timeout "$TMO" env -u OUT TMPDIR="$td" python3 -m pytest -q tests > "$o" 2>&1; rc=$?
    else nice -n "${HOSTTEST_NICE:-15}" timeout "$TMO" env -u OUT TMPDIR="$td" BUILD="$td" TEST_BUILD="$td" bash "$s" > "$o" 2>&1 < /dev/null; rc=$?; fi
    if [ $rc -eq 124 ]; then st=TIMEOUT
    elif [ $rc -eq 77 ] || tail -n 1 "$o" | grep -aq '^SKIP'; then st=SKIP
    elif [ $rc -eq 0 ]; then st=PASS
    elif grep -aqE 'build FAILED|error:|undefined reference|fatal error|cannot find|No such file|command not found|clang\+\+: |g\+\+: ' "$o" && ! grep -aqE '^(PASS|ok)' "$o"; then st=NOBUILD
    else st=FAIL; fi
  fi
  dt=$(( $(date +%s) - t0 )); [ $st = PASS ] && rm -f "$o"; rm -rf "$td"
  printf '%s\t%s\t%s\t%s\n' "$st" "$name" "$dt" "$(tail -n 1 "$o" 2>/dev/null | cut -c1-140)" >> "$RES"
  echo "$st $name (${dt}s)"
}
dupnames=$(printf '%s\n' $scripts | xargs -n1 basename 2>/dev/null | sed 's/\.sh$//' | sort | uniq -d | tr '\n' ' '); export dupnames
export -f run_one; export TMO OUT RES skiplist MSC
{ printf '%s\n' $scripts; [ $PYT = 1 ] && echo PYTEST; } | xargs -P "$JOBS" -I{} bash -c 'run_one {}'
cnt() { grep -c "^$1	" "$RES"; }
total=$(wc -l < "$RES"); pass=$(cnt PASS); skip=$(cnt SKIP); fail=$(cnt FAIL); nob=$(cnt NOBUILD); tmo=$(cnt TIMEOUT)
printf '\n%-8s %s\n' STATUS COUNT; printf '%-8s %s\n' PASS $pass SKIP $skip FAIL $fail NOBUILD $nob TIMEOUT $tmo TOTAL $total
[ $((fail + nob + tmo)) -gt 0 ] && { echo "Not green:"; grep -E "^(FAIL|NOBUILD|TIMEOUT)	" "$RES" | cut -f1,2,4; }
[ $skip -gt 0 ] && { echo "Skipped (not a pass):"; grep -E "^SKIP	" "$RES" | cut -f2,4 | head -30; }
if [ $WRITEQ = 1 ]; then awk -F'\t' '$1=="PASS" && $3<=8 {print $3"\t"$2}' "$RES" | sort -n | awk -F'\t' -v B=420 '{s+=$1; if (s<=B) print $2}' > tools/host_tests_quick.txt; echo "wrote tools/host_tests_quick.txt ($(wc -l < tools/host_tests_quick.txt) suites)"; fi
echo "(details: $RES, outputs of tests that did not pass: $OUT/<name>.out)"
[ $((fail + nob + tmo)) -eq 0 ] && [ "$total" -gt 0 ]
