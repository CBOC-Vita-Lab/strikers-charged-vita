#!/bin/bash
# Unit tests of the harness core against the mock game state (no game needed).
#   RUN="<qemu-arm ...>" BIN=<test_harness> TOOLS=<this directory> run_tests.sh
# Each case runs the test program with an environment and checks the log and the exit code.
BIN=${BIN:?}; RUN=${RUN:-}; TOOLS=${TOOLS:-$(dirname "$0")}
NAVDIR=${NAVDIR:-/home/claude/msc/tools/scripts}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok()   { pass=$((pass+1)); echo "ok   $1"; }
bad()  { fail=$((fail+1)); echo "FAIL $1"; echo "     $2"; }
# run <name> <expected exit code> <grep pattern>... -- env assignments, frames come from $FRAMES
run() {
  local name=$1 want=$2; shift 2
  local pats=(); while [ "$1" != "--" ]; do pats+=("$1"); shift; done; shift
  env "$@" $RUN "$BIN" ${FRAMES:-200} > "$T/out.txt" 2> "$T/err.txt"; local rc=$?
  cat "$T/out.txt" "$T/err.txt" > "$T/all.txt"
  if [ $rc != "$want" ]; then bad "$name" "exit $rc, wanted $want; stderr: $(tail -3 "$T/err.txt" | tr '\n' '|')"; return; fi
  for p in "${pats[@]}"; do
    grep -Eq -- "$p" "$T/all.txt" || { bad "$name" "missing /$p/"; return; }
  done
  ok "$name"
}
# W5 CTL: Vita-terms commands, repeat / endrepeat, try (soft timeouts)
cat > "$T/ctl.txt" <<'S'
repeat 2
vpress cross+r 2 1
endrepeat
vhold 2 circle 72 0 0 -72
touch 100 200 2 0
rswipe 200 300 260 300 3 0
try wait scene == 99 3
log after try
repeat 2
repeat 2
log inner
endrepeat
endrepeat
vmash square 4 f >= 30 100
log done
exit
S
FRAMES=100 run "W5 CTL: vpress/vhold/touch/rswipe/repeat/try/vmash" 0 \
  "raw f=1 buttons=4200" "raw f=4 buttons=4200" "raw f=7 buttons=2000 s=72,0 c=0,-72" "touch=1 [0-9]+@100,200" "rear=1 [0-9]+@200,300" \
  "rear=1 [0-9]+@260,300" "try line 7: timed out, going on" "\[script\] f=[0-9]+ after try" "raw f=[0-9]+ buttons=8000" "\[script\] f=[0-9]+ done" -- \
  MSC_INPUT_SCRIPT="$T/ctl.txt"
[ "$(grep -c '\[script\] f=[0-9]* inner' "$T/all.txt")" = 4 ] && ok "W5 CTL: nested repeat runs 4 times" || bad "W5 CTL: nested repeat" "$(grep -c inner "$T/all.txt") inner lines"
# W5 CTL: named checks (checkdef / expect / check, $name $namef $open)
cat > "$T/chk.txt" <<'S'
checkdef A 2 the frame counter goes up
checkdef B 2 the frame counter stays (it cannot)
checkdef D 3 never tried
expect f up
check A 10
if $A == 1
log A passed
endif
expect f same
check B 5
expect f same
check B 5
if $Bf == 2 && $open == 1
log B failed twice, one open
endif
exit
S
FRAMES=100 run "W5 CTL: checkdef/expect/check" 0 \
  "CHECK A PASS \(f up" "\[script\] f=[0-9]+ A passed" "CHECK B FAIL \(f same" "B failed twice, one open" "CHECK RESULT A PASS tries=1 fails=0" \
  "CHECK RESULT B FAIL tries=2 fails=2" "CHECK RESULT D NOT RUN" "checks: 1 passed, 1 failed, 1 not run \(of 3\)" -- MSC_INPUT_SCRIPT="$T/chk.txt"
FRAMES=5 run "no environment: harness inactive" 0 "start active=0" "loop ended after 5" -- MSC_NONE=1
FRAMES=100 run "MSC_MAX_FRAMES" 0 "reached MSC_MAX_FRAMES=7 at frame 7" -- MSC_MAX_FRAMES=7

cat > "$T/s1.txt" <<'S'
# absolute line: frames 1-3 hold A with stick (10,-10)
1 3 0100 10 -10
wait scene == 1
log in main menu
press 100 3 2
hold 4 0040 70 20 5 6
mash 100 10 scene == 5
log STATE match scene
wait gs == 5
stopwhen clock >= 2
hold 1000 0 0 0
S
FRAMES=300 run "script: wait/log/press/hold/mash/stopwhen" 0 \
  "\[script\] f=[0-9]+ in main menu" "\[script\] f=[0-9]+ STATE match scene" "stopwhen: clock >= 2" \
  "pad f=2 over=1 buttons=0100 s=10,-10" "pad f=1 over=0 buttons=0000" "buttons=0040 s=70,20 c=5,6 trig=255,0" "script stop condition at frame" -- MSC_INPUT_SCRIPT="$T/s1.txt"
# the A press: exactly 3 frames of 0100 after "in main menu", then 2 frames of gap
env MSC_INPUT_SCRIPT="$T/s1.txt" $RUN "$BIN" 300 2>/dev/null | awk '/^pad/ {print $2, $4}' > "$T/pads.txt"
python3 - "$T/pads.txt" <<'PY' && ok "script: press lasts 3 frames then neutral" || bad "script: press length" "see pads"
import sys
rows=[l.split() for l in open(sys.argv[1])]
a=[int(r[0].split('=')[1]) for r in rows if r[1]=='buttons=0100']
# frames 1-3 come from the absolute line; the press is the next run of 0100
runs=[];cur=[]
for f in a:
    if cur and f==cur[-1]+1: cur.append(f)
    else:
        if cur: runs.append(cur)
        cur=[f]
if cur: runs.append(cur)
assert len(runs)>=2 and len(runs[0])==3 and len(runs[1])==3, runs
PY

cat > "$T/s2.txt" <<'S'
wait scene == 99 50
S
FRAMES=300 run "script: wait timeout exits 3" 3 "\[script\] TIMEOUT f=[0-9]+ line 1: scene == 99 \(now" -- MSC_INPUT_SCRIPT="$T/s2.txt"
cat > "$T/s3.txt" <<'S'
wait scene === 1
S
FRAMES=10 run "script: parse error exits 2" 2 "cannot parse" -- MSC_INPUT_SCRIPT="$T/s3.txt"
cat > "$T/s4.txt" <<'S'
wait scene == 1 && nbuttons >= 8
if scene == 1
log branch taken
endif
if scene == 7
log branch NOT taken
endif
exit
S
FRAMES=300 run "script: && condition, if/endif, exit" 0 "branch taken" "script exit at frame" -- MSC_INPUT_SCRIPT="$T/s4.txt"
grep -q 'NOT taken' "$T/all.txt" && bad "script: if skips" "wrong branch taken" || ok "script: if skips the untaken branch"

FRAMES=120 run "MSC_WATCH: [nav] changes and [watch] lines" 0 "\[nav\] f=[0-9]+ scenes: \[18:scene18\]" "\[nav\] f=[0-9]+ match state -1 -> 1" \
  "\[nav\] f=[0-9]+ score 0-0 -> 1-0" "\[nav\] f=[0-9]+ pause menu on" "\[watch\] f=60 " "sim=[0-9]+ simt=" -- MSC_WATCH=1 MSC_WATCH_EVERY=60
FRAMES=30 run "MSC_WATCH extras (symbol expression)" 0 "tv=12.5000" -- "MSC_WATCH=tv:f32:testVar" MSC_WATCH_EVERY=10
FRAMES=30 run "MSC_WATCH_BUTTONS lists buttons" 0 "\[nav\] f=[0-9]+ buttons=8 pointer=" -- MSC_WATCH_BUTTONS=1
FRAMES=3 run "MSC_FRAME_TRACE" 0 "\[ft\] 1 0 1 -1" -- MSC_FRAME_TRACE=1

# virtual time: same log twice (except real-time fields), first clock read, per-frame charge
vt() { env MSC_FRAME_LOG=0 MSC_VIRTUAL_TIME=1 MSC_WATCH=1 MSC_WATCH_EVERY=20 MSC_STATE_HASH=25 "$@" $RUN "$BIN" 100 2>&1 | sed -E 's/ rt=[0-9.]+//; s/t=[0-9.]+s//' ; }
vt MSC_X=1 > "$T/v1.txt"; vt MSC_X=1 > "$T/v2.txt"
if cmp -s "$T/v1.txt" "$T/v2.txt" && grep -q '\[hash\] f=25 sim=' "$T/v1.txt"; then ok "virtual time: two runs identical, hash lines present"; else bad "virtual time: repeatability" "$(diff "$T/v1.txt" "$T/v2.txt" | head -3)"; fi
grep -q 't=1000000001000' "$T/v1.txt" && ok "virtual time: monotonic clock starts at 1000 s, 1 us per read" || bad "virtual time start" "$(grep -m2 'pad f=1 ' "$T/v1.txt")"
# a clock read before the harness' static initialisers (a library constructor) must not make the main thread "not the game thread"
env MSC_VIRTUAL_TIME=1 MSC_EARLY_READ=1 $RUN "$BIN" 2 2>/dev/null | awk '/^pad f=(1|2) / {print $NF}' | tr '\n' ' ' > "$T/early.txt"
if grep -q '^t=1000000000000 t=1000000001000 *$' "$T/early.txt"; then ok "virtual time: an early library clock read keeps the main thread the game thread"; else bad "early clock read" "got: $(cat "$T/early.txt")"; fi
env MSC_VIRTUAL_TIME=1 MSC_VIRTUAL_FRAME_US=1000 $RUN "$BIN" 4 2>/dev/null | awk '/^pad f=4 / {print $NF}' > "$T/t4.txt"
want=$((1000000000000 + 3*1000000 + 3*1000))   # three frames of 1 ms charged, plus one 1 us read per earlier pad line
t4=$(sed 's/t=//' "$T/t4.txt")
[ "$t4" = "$want" ] && ok "virtual time: MSC_VIRTUAL_FRAME_US charges per frame" || bad "MSC_VIRTUAL_FRAME_US" "t4=$t4 want~$want"

# every existing nav script of the recompiled build parses
for s in "$NAVDIR"/*.txt; do
  [ -f "$s" ] || continue
  if env MSC_SCRIPT_CHECK=1 MSC_INPUT_SCRIPT="$s" $RUN "$BIN" 1 > "$T/chk.txt" 2>&1 && grep -q '\[script\] OK' "$T/chk.txt"; then ok "parses $(basename "$s"): $(grep -o '[0-9]* absolute.*' "$T/chk.txt")"; else bad "parse $(basename "$s")" "$(head -2 "$T/chk.txt")"; fi
done
echo "---- $pass passed, $fail failed"
[ $fail = 0 ]
