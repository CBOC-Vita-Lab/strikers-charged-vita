#!/bin/bash
# Records the profile for NP_CLANG_PGO: instrumented armhf harness build, scripted routes under qemu-arm, llvm-profdata merge.
#   NP_DISC_DIR=<game files> SAVE_DIR=<data dir with a save> tools/pgo/record.sh <build root> <out.profdata>
# Needs: clang 18, llvm-profdata-18, arm-linux-gnueabihf g++ + sysroot, qemu-arm (J=<make jobs>, default 2); the profile runtime from tools/pgo/build_runtime.sh
# (RT=<libclang_rt.profile-armhf.a>). Routes: ROUTES="a.txt:4600 b.txt:14000" (script:max frames); defaults are the two below.
# Wrap in tools/heavy.sh: a build plus two runs take about 15 minutes on two cores. The build is -O2 ThinLTO with the Vita's code-generation
# flags; the profile only matches game objects built from the same sources (a source change makes affected functions fall back to no profile).
# Wave 4: VGX_PGO=1 instruments the renderer library too (the Vita build then takes VGX_PGO=1 NP_CLANG_PGO=<profile rebased with tools/pgo/rebase.sh>);
#   PGO_ENV=<file of NAME=VALUE lines> runs the routes with those switches (the Vita defaults: vita_plan.cpp kSpeed/kQuality).
# 14 Nov 2026 (pgo10): TGT=host32 records with the host32 harness (clang -m32, glnull back end) and the profile runtime profrt_min.c (no compiler-rt, no armhf
# sysroot, no qemu needed). Checked: the CFG hashes (name + FuncHash of every __profd_) of host32 and Vita objects are equal (99% of functions in the TUs tried),
# so the profile drives the Vita build; see w8/pgo10.md.   TGT=host32 NP_DISC_DIR=<game files> SAVE_DIR=<dir> tools/pgo/record.sh <build root> <out.profdata>
set -e
TGT=${TGT:-armhf}
B=$(realpath -m "${1:?build root}"); OUTP=$(realpath -m "${2:?out.profdata}")
cd "$(dirname "$(readlink -f "$0")")/../.."          # native/
[ "$TGT" = host32 ] && RT=${RT:-$B/profrt_min.o}
RT=${RT:-$(realpath -m ../build/pgo-runtime/libclang_rt.profile-armhf.a)}; [ "$TGT" = host32 ] || [ -f "$RT" ] || { echo "profile runtime missing: $RT (tools/pgo/build_runtime.sh)"; exit 1; }
: "${NP_DISC_DIR:?}" "${SAVE_DIR:?}"; QEMU=${QEMU:-qemu-arm}
# 1.3 (w7): the routes cover menus, the walk-out, open play, goals with celebration and replay, Mega Strikes and Mega Saves in the default stadium and in
# stadium 13 (replay, megastrike); frame limits are pictures at 60 Hz (MSC_FPS=60 below), a route ends at its own last line if that comes first.
ROUTES=${ROUTES:-"platform/vita/session/nav/match60.txt:4800 ../tools/console-1.2/routes/tour.txt:6500 ../tools/console-1.2/routes/replay.txt:12000 tools/routes/mega_strike_route.txt:8000 tools/pgo/megastrike.txt:9000 tools/routes/mega_route.txt:4000"}
NATIVE=$PWD; mkdir -p "$B"
if [ "${REC_STEPS:-all}" = run ]; then if [ "$TGT" = host32 ]; then EXE=$B/host32/np-host32-full-glnull; QEMU=; else EXE=$B/armhf/np-armhf-full; fi
elif [ "$TGT" = host32 ]; then
  clang -m32 -O2 -c tools/pgo/profrt_min.c -o "$RT"
  EXE=$B/host32/np-host32-full-glnull; QEMU=
  make -j${J:-2} NP_PGO_ALL=${NP_PGO_ALL:-1} TARGET=host32 WITH_SDK=1 WITH_GX=1 WITH_PLAT=1 GX_BACKEND=glnull GX_VGX=$(realpath ../renderer) GX_I386ROOT=/ \
    EXESUF=-full-glnull BUILD_ROOT="$B" DEBUG=-g0 "OPT=-O2 -ffunction-sections -fdata-sections" NP_CLANG_PGO_GEN=1 \
    "LDEXTRA=$RT -Wl,-u,__llvm_profile_runtime -Wl,-u,__llvm_profile_write_file -Wl,-u,__llvm_profile_instrument_target -Wl,-u,__llvm_profile_instrument_memop -Wl,-u,__llvm_profile_instrument_range"
else
  EXE=$B/armhf/np-armhf-full
# the instrumented objects reference nlLocalization::GetString out of line and the LTO code generator adds the value-profile calls late:
# a weak stub and -u flags keep the link complete (the stub is never reached by the scripted routes)
STUB=$B/pgo_stub.o
printf 'int s(void) __asm__("_ZNK14nlLocalization9GetStringEPKc");\n__attribute__((weak)) int s(void){return 0;}\n' | \
  clang --target=arm-linux-gnueabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -O1 -x c -c - -o "$STUB"
clang --target=arm-linux-gnueabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -O1 -c tools/perf/extra_stubs.c -o "$B/extra_stubs.o"
# NP_PGO_ALL=1 (default here): renderer library, GX glue and platform layer instrumented too; NP_PGO_ALL=0 records the game's src/ alone
make -j${J:-2} NP_PGO_ALL=${NP_PGO_ALL:-1} TARGET=armhf NOPIC=1 "OPT=-O2 -ffunction-sections -fdata-sections" WITH_SDK=1 WITH_GX=1 WITH_PLAT=1 GX_VGX=$(realpath ../renderer) \
  EXESUF=-full BUILD_ROOT="$B" DEBUG=-g0 NP_CLANG_PGO_GEN=1 \
  "LDEXTRA=$B/extra_stubs.o $STUB $RT -Wl,-u,__llvm_profile_runtime -Wl,-u,__llvm_profile_write_file -Wl,-u,__llvm_profile_instrument_target -Wl,-u,__llvm_profile_instrument_memop -Wl,-u,__llvm_profile_instrument_range"
fi
[ "${REC_STEPS:-all}" = build ] && { echo "built: $EXE"; exit 0; }      # REC_STEPS=build|run|all: build under hostbuild.sh, run the routes under the heavy lock
# the switches the Vita build turns on by itself (vita_plan.cpp) and the 60 Hz thread layout, unless PGO_ENV names a file (PGO_ENV=/dev/null: none)
if [ -z "${PGO_ENV:-}" ]; then PGO_ENV=$B/pgo.env; python3 "$NATIVE/tools/pgo/vita_env.py" > "$PGO_ENV"; fi
VFU=33000; grep -q '^MSC_FPS=60' "$PGO_ENV" && VFU=2000      # 60 Hz pictures: the virtual frame waits for the retrace (as the cpu60 harness)
FSEND=nofork; grep -q '^MSC_FRAMESEND=' "$PGO_ENV" && FSEND=
[ -n "$KEEP_RAW" ] || rm -rf "$B/raw"; mkdir -p "$B/raw"
for r in $ROUTES; do
  s=${r%%:*}; n=${r##*:}; t=$(basename "$s" .txt); D=$B/data-$t; W=1; [ "$t" = mega_route ] && W="keepstate:s32:[[g_pTeams]+B4]+334"; rm -rf "$D"; cp -a "$SAVE_DIR" "$D"
  ( cd "$D" && env $( [ -n "${PGO_ENV:-}" ] && grep -v "^#" "$PGO_ENV" ) NP_DISC_DIR=$NP_DISC_DIR NP_DATA_DIR=$D MSC_INPUT_SCRIPT=$NATIVE/$s MSC_MAX_FRAMES=$n MSC_VIRTUAL_TIME=1 MSC_VIRTUAL_FRAME_US=$VFU \
    ${FSEND:+MSC_FRAMESEND=$FSEND} "MSC_WATCH=$W" MSC_WATCH_EVERY=600 LLVM_PROFILE_FILE=$B/raw/$t-%p.profraw \
    ${QEMU:+$QEMU -cpu cortex-a9 -L /usr/arm-linux-gnueabihf} "$EXE" > "$B/run-$t.log" 2>&1 ) || true
  echo "$(date +%T) route $t done: $(ls "$B"/raw/$t-*.profraw 2>/dev/null | wc -l) profraw, $(grep -c script "$B/run-$t.log") script lines" >> "$B/progress.txt"
  rm -rf "$D"
done
llvm-profdata-18 merge -o "$OUTP" "$B"/raw/*.profraw
ls -la "$OUTP"
