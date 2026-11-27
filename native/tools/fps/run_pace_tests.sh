#!/bin/bash
# 1.3 pacing: MSC_FPS_BETWEEN and MSC_PRESENT_SYNC in the real np_fps.cpp, the pipeline model, and the start-up clock rules. No game data, about a minute.
#   native/tools/fps/run_pace_tests.sh
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${OUT:-${TMPDIR:-/tmp}/np-pace-tests}
mkdir -p "$out"
CXX=${CXX:-g++}
san=${SAN:--fsanitize=address,undefined -fno-sanitize-recover=undefined}
$CXX -std=c++17 -Wall -Wextra $san -Inative/platform/vita/src native/platform/vita/tests/test_clock_policy.cpp native/tools/fps/gsplit_stub.cpp -o "$out/test_clock_policy"
"$out/test_clock_policy"
$CXX -std=c++11 -O1 -Wall $san -Inative/include -Inative/platform/compat native/tools/fps/fps_mid_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/fps_mid_unit"
env -i PATH="$PATH" MSC_FPS=auto MSC_FPS_BETWEEN=pace MSC_SLIP=20 "$out/fps_mid_unit" behaviour 2>/dev/null
plain=$(env -i PATH="$PATH" MSC_FPS=auto MSC_SLIP=20 "$out/fps_mid_unit" trace 2>/dev/null)
paced=$(env -i PATH="$PATH" MSC_FPS=auto MSC_SLIP=20 MSC_FPS_BETWEEN=pace MSC_PRESENT_SYNC=2 "$out/fps_mid_unit" trace 2>/dev/null)
echo "LAN session: without the switches $plain, with them $paced"
[ "$plain" = "$paced" ] || { echo "pace tests: FAILED (the switches changed a LAN session)"; exit 1; }
$CXX -std=c++17 -O2 -Wall -Wextra -Irenderer/src -Inative/platform/compat native/tools/fps/pace_mid_model.cpp native/tools/fps/gsplit_stub.cpp -o "$out/pace_mid_model"
"$out/pace_mid_model"
# wave 9: the hold-mix, frames-in-flight depth and ladder-budget checks of the model, and their mutants (each must fail), the sticky gate and its mutants
for m in "PACE_BREAK=1" "PACE_BREAK=2" "VGX_UPBUDGET_BREAK=1" "VGX_UPBUDGET_BREAK=2"; do
    $CXX -std=c++17 -O1 -D$m -Irenderer/src -Inative/platform/compat native/tools/fps/pace_mid_model.cpp native/tools/fps/gsplit_stub.cpp -o "$out/pace_mid_model_break" 2>/dev/null
    if "$out/pace_mid_model_break" >/dev/null 2>&1; then echo "pace tests: FAILED (mutant $m not caught)"; exit 1; fi
    echo "pace_mid_model: mutant $m is caught"
done
$CXX -std=c++11 -Wall -Inative/platform/compat native/tools/fps/between_sticky_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/between_sticky_unit"
"$out/between_sticky_unit"
for m in 1 2 3 4; do
    $CXX -std=c++11 -DNP_BETWEEN_BREAK=$m -Inative/platform/compat native/tools/fps/between_sticky_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/between_sticky_break"
    if "$out/between_sticky_break" >/dev/null 2>&1; then echo "pace tests: FAILED (between mutant $m not caught)"; exit 1; fi
    echo "between_sticky_unit: mutant $m is caught"
done
# wave 11 (press16): the pad read aligned to the controller's samples (np_lat16.h) on a model of the held 30, and its mutants
$CXX -std=c++17 -O2 -Wall -Wextra -Inative/platform/compat native/tools/fps/lat16_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/lat16_unit"
"$out/lat16_unit"
for m in 1 2 3 4 5; do
    $CXX -std=c++17 -O2 -DNP_LAT16_BREAK=$m -Inative/platform/compat native/tools/fps/lat16_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/lat16_unit_break"
    if "$out/lat16_unit_break" >/dev/null 2>&1; then echo "pace tests: FAILED (lat16 mutant $m not caught)"; exit 1; fi
    echo "lat16_unit: mutant $m is caught"
done
$CXX -std=c++11 -Wall -Inative/platform/compat native/tools/fps/pair_rate_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/pair_rate_unit"
"$out/pair_rate_unit"
for m in 1 2 3 4 5 6 7; do
    $CXX -std=c++11 -DNP_PAIR_BREAK=$m -Inative/platform/compat native/tools/fps/pair_rate_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/pair_rate_break"
    if "$out/pair_rate_break" >/dev/null 2>&1; then echo "pace tests: FAILED (pair mutant $m not caught)"; exit 1; fi
    echo "pair_rate_unit: mutant $m is caught"
done
# lessons17: the late-picture attribution (np_late_cause.h) and its mutants
$CXX -std=c++11 -Wall -Inative/platform/compat native/tools/fps/late_cause_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/late_cause_unit"
"$out/late_cause_unit" | tail -1
for m in 1 2 3 4; do
    $CXX -std=c++11 -DNP_LATECAUSE_BREAK=$m -Inative/platform/compat native/tools/fps/late_cause_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/late_cause_break"
    if "$out/late_cause_break" >/dev/null 2>&1; then echo "pace tests: FAILED (late cause mutant $m not caught)"; exit 1; fi
    echo "late_cause_unit: mutant $m is caught"
done
$CXX -std=c++11 -Wall -Inative/platform/compat native/tools/fps/fif_drain_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/fif_drain_unit"
"$out/fif_drain_unit"
for m in 1 2 3; do
    $CXX -std=c++11 -DNP_FIF_BREAK=$m -Inative/platform/compat native/tools/fps/fif_drain_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/fif_drain_break"
    if "$out/fif_drain_break" >/dev/null 2>&1; then echo "pace tests: FAILED (fif mutant $m not caught)"; exit 1; fi
    echo "fif_drain_unit: mutant $m is caught"
done
$CXX -std=c++11 -Wall -Inative/platform/compat native/tools/fps/free_cost_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/free_cost_unit"
"$out/free_cost_unit"
for m in 1 2 3; do
    $CXX -std=c++11 -DNP_FREECOST_BREAK=$m -Inative/platform/compat native/tools/fps/free_cost_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/free_cost_break"
    if "$out/free_cost_break" >/dev/null 2>&1; then echo "pace tests: FAILED (free cost mutant $m not caught)"; exit 1; fi
    echo "free_cost_unit: mutant $m is caught"
done
$CXX -std=c++17 -O2 -Wall -Wextra -Inative/platform/compat native/tools/fps/pace10_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/pace10_unit"
"$out/pace10_unit"
for m in NP_PHASE_BREAK=1 NP_PHASE_BREAK=2 NP_PHASE_BREAK=3 NP_PHASE_BREAK=4 NP_PHASE_BREAK=5 PACE10_BREAK=1; do
    $CXX -std=c++17 -O2 -D$m -Inative/platform/compat native/tools/fps/pace10_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/pace10_unit_break"
    if "$out/pace10_unit_break" >/dev/null 2>&1; then echo "pace tests: FAILED (pace10 mutant $m not caught)"; exit 1; fi
    echo "pace10_unit: mutant $m is caught"
done
# helper f45: the 45 FPS cadence (np_fps45.h): pattern held, phase kept, show-time game time, fallback hysteresis; its mutants (a wandering pattern, a wrong alpha, a lead that never goes up or comes down unproven, no slow fallback window, ...)
$CXX -std=c++11 -O1 -Wall -Wextra -Inative/platform/compat native/tools/fps/f45_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/f45_unit"
"$out/f45_unit"
for m in 1 2 3 4 5 6 7 8 9 10; do
    $CXX -std=c++11 -O1 -DNP_F45_BREAK=$m -Inative/platform/compat native/tools/fps/f45_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/f45_unit_break"
    if "$out/f45_unit_break" >/dev/null 2>&1; then echo "pace tests: FAILED (f45 mutant $m not caught)"; exit 1; fi
    echo "f45_unit: mutant $m is caught"
done
# f50 (24 Nov, MSC_LOCK60_USE50): the same model built for the strict 50 (5 pictures in 6 refreshes) and its 1..10 mutants; then f50_unit (pattern, gate, clock, tier choice, ladder 60 -> 50 -> 30, trial / proof back, seams) and its mutants
$CXX -std=c++11 -O1 -Wall -Wextra -DNP_UNIT_N=5 -DNP_UNIT_M=6 -Inative/platform/compat native/tools/fps/f45_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/f45_unit50"
"$out/f45_unit50"
for m in 1 2 3 4 5 6 7 8 9 10; do
    $CXX -std=c++11 -O1 -DNP_F45_BREAK=$m -DNP_UNIT_N=5 -DNP_UNIT_M=6 -Inative/platform/compat native/tools/fps/f45_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/f45_unit50_break"
    if "$out/f45_unit50_break" >/dev/null 2>&1; then echo "pace tests: FAILED (f45 mutant $m at 50 not caught)"; exit 1; fi
    echo "f45_unit (50): mutant $m is caught"
done
$CXX -std=c++11 -O1 -Wall -Inative/platform/compat native/tools/fps/f50_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/f50_unit"
"$out/f50_unit" | tail -1
for m in F45:11 F45:12 F45:13 F50:1 F50:2 F50:3 F50:4 F50:5; do
    $CXX -std=c++11 -O1 -DNP_${m%%:*}_BREAK=${m##*:} -Inative/platform/compat native/tools/fps/f50_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/f50_unit_break"
    if "$out/f50_unit_break" >/dev/null 2>&1; then echo "pace tests: FAILED (f50 mutant $m not caught)"; exit 1; fi
    echo "f50_unit: mutant $m is caught"
done
$CXX -std=c++11 -Wall -Inative/platform/compat native/tools/fps/bp_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/bp_unit"
"$out/bp_unit"
for m in 1 2 3 4 5 6 7 8 9 10 11; do
    $CXX -std=c++11 -DNP_BP_BREAK=$m -Inative/platform/compat native/tools/fps/bp_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/bp_unit_break"
    if "$out/bp_unit_break" >/dev/null 2>&1; then echo "pace tests: FAILED (back-pressure mutant $m not caught)"; exit 1; fi
    echo "bp_unit: mutant $m is caught"
done
$CXX -std=c++11 -Wall -Inative/platform/compat native/tools/fps/game_clock_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/game_clock_unit"
"$out/game_clock_unit"
for m in 1 2 3 4 5; do
    $CXX -std=c++11 -DNP_CLOCK_BREAK=$m -Inative/platform/compat native/tools/fps/game_clock_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/game_clock_break"
    if "$out/game_clock_break" >/dev/null 2>&1; then echo "pace tests: FAILED (game clock mutant $m not caught)"; exit 1; fi
    echo "game_clock_unit: mutant $m is caught"
done
$CXX -std=c++11 -O1 -DNP_NATIVE=1 -Inative/include -Inative/platform/compat native/tools/fps/clock_log_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/clock_log_unit"
"$out/clock_log_unit" debt 2>/dev/null
"$out/clock_log_unit" slip 2>/dev/null
$CXX -std=c++11 -Inative/include native/tools/fps/framepace_paced_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/framepace_paced_unit"
MSC_PACE_VBLANK=1 "$out/framepace_paced_unit"
$CXX -std=c++11 -O1 -Wall -Inative/platform/compat -Inative/include native/tools/fps/smooth_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/smooth_unit"
"$out/smooth_unit"
for m in 1 2 3 4; do
    $CXX -std=c++11 -O1 -DNP_SHOW_BREAK=$m -Inative/platform/compat -Inative/include native/tools/fps/smooth_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/smooth_break"
    if "$out/smooth_break" >/dev/null 2>&1; then echo "pace tests: FAILED (show clock mutant $m not caught)"; exit 1; fi
    echo "smooth_unit: mutant $m is caught"
done
# Fable (MSC_LOCK60_TIERS): Smooth 60 in three tiers (np_fps_tier.h, np_fps_ctl.h Lock60Tiered, np_fps45.h): spikes tolerated, a sustained shortfall -> 45, 45 -> 30 only when 45 fails,
# no flapping, up in order on proof, the show-time clock at 60; mutants: falls on a spike, never falls, flaps, skips 45
$CXX -std=c++11 -O1 -Inative/platform/compat native/tools/fps/tier_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/tier_unit"
"$out/tier_unit"
for m in 1 2 3 4; do
    $CXX -std=c++11 -O1 -DNP_TIER_BREAK=$m -Inative/platform/compat native/tools/fps/tier_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/tier_unit_break"
    if "$out/tier_unit_break" >/dev/null 2>&1; then echo "pace tests: FAILED (tier mutant $m not caught)"; exit 1; fi
    echo "tier_unit: mutant $m is caught"
done
# hold60 (MSC_LOCK60_HOLD): 60 judged on the delivered rate, the climb on measured work, the 45 / 30 hold rules, console traces (hold60_traces.txt), motion at 60 with repeats;
# mutants: never falls, falls on a spike, flaps, never climbs, climbs on the frame length, the 45's old fast window, the 30 never climbs; hold60b: 8 a full step on every picture, 9 the climb
# without the blend lane's wait, NP_SPREAD_BREAK=1 the 60's catch-up not spread
$CXX -std=c++11 -O1 -Inative/platform/compat native/tools/fps/hold60_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/hold60_unit"
"$out/hold60_unit" x native/tools/fps/hold60_traces.txt
for m in NP_HOLD_BREAK=1 NP_HOLD_BREAK=2 NP_HOLD_BREAK=3 NP_HOLD_BREAK=4 NP_HOLD_BREAK=5 NP_HOLD_BREAK=6 NP_HOLD_BREAK=7 NP_HOLD_BREAK=8 NP_HOLD_BREAK=9 NP_SPREAD_BREAK=1; do
    $CXX -std=c++11 -O1 -D$m -Inative/platform/compat native/tools/fps/hold60_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/hold60_break"
    if "$out/hold60_break" x native/tools/fps/hold60_traces.txt >/dev/null 2>&1; then echo "pace tests: FAILED (hold60 mutant $m not caught)"; exit 1; fi
    echo "hold60_unit: mutant $m is caught"
done
# pace13: the flap model (MSC_TIER_DWELL / MSC_TIER_RELAPSE on hovering loads, Galaxy-style governor beside it); mutants: dwell ignored, relapse backoff never counts
$CXX -std=c++11 -O1 -Inative/platform/compat native/tools/fps/tier_flap_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/tier_flap_unit"
"$out/tier_flap_unit"
for m in 1 2; do
    $CXX -std=c++11 -O1 -DNP_FLAP_BREAK=$m -Inative/platform/compat native/tools/fps/tier_flap_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/tier_flap_break"
    if "$out/tier_flap_break" >/dev/null 2>&1; then echo "pace tests: FAILED (flap mutant $m not caught)"; exit 1; fi
    echo "tier_flap_unit: mutant $m is caught"
done
# pace13: MSC_TIER_MEMORY (np_fps_tiermem.h + the seeded start of the real controller / Fallback45); mutants: wrong settled tier, a 60 keeps the entry, key without the mode, the seeded start never steps up
$CXX -std=c++11 -O1 -Inative/platform/compat native/tools/fps/tiermem_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/tiermem_unit"
"$out/tiermem_unit"
for m in 1 2 3 4; do
    $CXX -std=c++11 -O1 -DNP_TMEM_BREAK=$m -Inative/platform/compat native/tools/fps/tiermem_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/tiermem_break"
    if "$out/tiermem_break" >/dev/null 2>&1; then echo "pace tests: FAILED (tiermem mutant $m not caught)"; exit 1; fi
    echo "tiermem_unit: mutant $m is caught"
done
# wave 24 (feel24): the gameplay camera at 60 / 45 / 30 through the game's Dampen: the console's 45 aim-age figure is the damper's input age, not a motion error; its mutant (no damping) must fail
$CXX -std=c++11 -O1 -Wall -Inative/include native/tools/fps/cam45_unit.cpp -o "$out/cam45_unit"
"$out/cam45_unit" | tail -1
$CXX -std=c++11 -O1 -DCAM45_BREAK=1 -Inative/include native/tools/fps/cam45_unit.cpp -o "$out/cam45_unit_break"
if "$out/cam45_unit_break" >/dev/null 2>&1; then echo "pace tests: FAILED (cam45 mutant not caught)"; exit 1; fi
echo "cam45_unit: mutant is caught"
# pace24 (wave 24): the cut-in grace and the 2 s window (MSC_LOCK60_FASTGRACE), the climb's step in the frame's clock (MSC_HOLD60_WALLSIM), the 45's fallback on late pictures and the delivered rate
# (MSC_F45_LATEONLY), the spread's debt cap (MSC_LOCK60_SPREAD_CAP); all default off; mutants: 1 a collapse never falls in the grace, 2 the grace does nothing, 3 the wall sim priced with CPU ms,
# 4 the rate rule never speaks, 5 it is inverted, NP_SPREAD_BREAK=2 the debt cap ignored
$CXX -std=c++11 -O1 -Inative/platform/compat native/tools/fps/pace24_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/pace24_unit"
"$out/pace24_unit"
for m in NP_P24_BREAK=1 NP_P24_BREAK=2 NP_P24_BREAK=3 NP_P24_BREAK=4 NP_P24_BREAK=5 NP_SPREAD_BREAK=2; do
    $CXX -std=c++11 -O1 -D$m -Inative/platform/compat native/tools/fps/pace24_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/pace24_break"
    if "$out/pace24_break" >/dev/null 2>&1; then echo "pace tests: FAILED (pace24 mutant $m not caught)"; exit 1; fi
    echo "pace24_unit: mutant $m is caught"
done
# trap24 (wave 24): the 45's fallback 30 that never ends (console 6024): the proof judged on the mean cost after the hold (MSC_F45_RETRY, default off); mutants: 1 the mean is not judged, 2 no wait after the hold, 3 a lenient mean
$CXX -std=c++11 -O1 -Inative/platform/compat native/tools/fps/trap24_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/trap24_unit"
"$out/trap24_unit"
for m in NP_T24_BREAK=1 NP_T24_BREAK=2 NP_T24_BREAK=3; do
    $CXX -std=c++11 -O1 -D$m -Inative/platform/compat native/tools/fps/trap24_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/trap24_break"
    if "$out/trap24_break" >/dev/null 2>&1; then echo "pace tests: FAILED (trap24 mutant $m not caught)"; exit 1; fi
    echo "trap24_unit: mutant $m is caught"
done
# p98 (wave 24): MSC_LOCK60_P98 (60 held on the repeated refreshes and visible hitches the player sees, a 2 s spike keeps 60) and MSC_LOCK60_TRIAL (45 -> 60 and 30 -> 45 by trial, backoff 4 8 16 20, cuts, GPU-bound stays);
# mutants: 1 no sustain, 2 never falls, 3 no hitch rule, 4 a trial never keeps, 5 the backoff never grows, 6 no trial at a cut, 7 a GPU-bound relief is retried, 8 USE45 ignored
$CXX -std=c++11 -O1 -Inative/platform/compat native/tools/fps/p98_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/p98_unit"
"$out/p98_unit"
for m in 1 2 3 4 5 6 7 8; do
    $CXX -std=c++11 -O1 -DNP_P98_BREAK=$m -Inative/platform/compat native/tools/fps/p98_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/p98_break"
    if "$out/p98_break" >/dev/null 2>&1; then echo "pace tests: FAILED (p98 mutant $m not caught)"; exit 1; fi
    echo "p98_unit: mutant $m is caught"
done
# wave 25 (flip25): the trial of 60 by time: the defer (30 -> 50 -> 60) and the calm rules, and their mutants
$CXX -std=c++11 -O1 -Wall -Inative/platform/compat native/tools/fps/flip25_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/flip25_unit" 2>/dev/null
"$out/flip25_unit"
for m in 1 2 3 4; do
    $CXX -std=c++11 -O1 -DNP_FLIP_BREAK=$m -Inative/platform/compat native/tools/fps/flip25_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/flip25_unit_break" 2>/dev/null
    if "$out/flip25_unit_break" >/dev/null 2>&1; then echo "pace tests: FAILED (flip25 mutant $m not caught)"; exit 1; fi
    echo "flip25_unit: mutant $m is caught"
done
# nxr4c (25 Nov): the console's [hold60] windows (6027 / 6032 / 6033 / 6034) replayed through the real controller, MSC_HOLD60_WALLSIM (now default on) against off; mutant: the wall sim priced with CPU ms
$CXX -std=c++11 -O1 -Inative/platform/compat native/tools/fps/wallsim25_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/wallsim25_unit"
"$out/wallsim25_unit"
$CXX -std=c++11 -O1 -DNP_P24_BREAK=3 -Inative/platform/compat native/tools/fps/wallsim25_unit.cpp native/tools/fps/gsplit_stub.cpp -o "$out/wallsim25_break"
if "$out/wallsim25_break" >/dev/null 2>&1; then echo "pace tests: FAILED (wallsim25 mutant not caught)"; exit 1; fi
echo "wallsim25_unit: mutant is caught"
echo "pace tests: passed"
