#!/bin/bash
# tools/refcheck.sh <tag> [exe]: the two regression references of the front end, null back end,
# scripts/survey_full.txt (8457 frames each, no heavy lock, about 1.5 minutes per run):
#   compat : MSC_VGX_HASH=1 ABSIDX=0 DXT1=0 ATLAS=0 UNIPACK=0 LIGHTNORM=0 PALIDX=0  against build/host-gl/s6
#            (the GL survey of e934e1b, whose pictures were looked at: everything a switch of
#            rounds 4 and 5 changes is off, so this is the chain back to the checked pictures)
#   def    : MSC_VGX_HASH=1 (default configuration)            against checks/r6/ref_def
#            (since round 6: the palette row index is int(floor(w + 0.5)) in the generated
#            vertex shaders, MSC_VGX_PALIDX; the hash covers shader text, so the 13 frames
#            that generate such a shader differ from the round 5 reference checks/r5/ref_def,
#            which the default configuration still reproduces with MSC_VGX_PALIDX=0. That
#            one came with the light-direction normalisation of round 5; before it
#            checks/r3/a2_def, still reproduced with MSC_VGX_LIGHTNORM=0 MSC_VGX_PALIDX=0)
# Both references were made with the FROZEN BASELINE game objects (frozen/host). A build of
# the game code that leaves other bytes in dead stack slots can differ from them where the
# game draws with uninitialised data that cannot reach a pixel: see NOTES.md, "Hash reference".
# Output in build/host-gl/checks/r5/<tag>_{compat,def}; exit status 0 only if both are identical.
# REFCHECK_ONLY=compat|def|def5 runs one of them (def5: MSC_VGX_PALIDX=0 against checks/r5/ref_def).
# REFCHECK_FRAMES=n compares only the first n frames (3300 = boot, menus, loading, 450 frames of
# play: 20 s per run, short enough to run without the heavy lock while the queue is long). It is a
# quick look, NOT the check a commit needs: the whole survey goes through tools/heavy.sh.
# Since the 9 Oct merge (branch `one`): runs from the checkout it lives in (scripts/, tools/, run_nav_gl.sh
# of THIS tree), output under $REFCHECK_OUT (default build/host-gl/checks/r5), references under
# $REFCHECK_REFS (default build/host-gl/checks) and REFCHECK_EXTRA="VAR=1 VAR2=0" adds variables to both runs
# (the new switches of the merge: off must reproduce the references, defaults are compared with REFCHECK_DEF_REF).
MSC=/home/claude/msc; C=${REFCHECK_REFS:-$MSC/build/host-gl/checks}; OUTD=${REFCHECK_OUT:-$C/r5}
TAG=${1:?tag}; EXE=${2:-$MSC/build/host-gl/msc_null_hle}
cd "$(dirname "$(readlink -f "$0")")/.."; mkdir -p $OUTD; rc=0
run() { # name reference env...
  local n=$1 ref=$2; shift 2
  env "$@" $REFCHECK_EXTRA MSC_VGX_HASH=1 VGX_NOLOCK=1 VGX_BIN=$EXE ./run_nav_gl.sh scripts/survey_full.txt $OUTD/${TAG}_$n 0 ${REFCHECK_FRAMES:-0} > $OUTD/${TAG}_$n.log 2>&1
  python3 tools/cmp_hash.py $ref $OUTD/${TAG}_$n/vgx_frames.csv | head -4 | sed "s/^/$n: /"
  [ ${PIPESTATUS[0]} = 0 ] || rc=1
}
if [ "$REFCHECK_ONLY" = def5 ]; then run def5 $C/r5/ref_def/vgx_frames.csv MSC_VGX_PALIDX=0; exit $rc; fi
DEFREF=${REFCHECK_DEF_REF:-$C/r6/ref_def/vgx_frames.csv}
[ "$REFCHECK_ONLY" = def ] || run compat $MSC/build/host-gl/s6/vgx_frames.csv MSC_VGX_ABSIDX=0 MSC_VGX_DXT1=0 MSC_VGX_ATLAS=0 MSC_VGX_UNIPACK=0 MSC_VGX_LIGHTNORM=0 MSC_VGX_PALIDX=0
[ "$REFCHECK_ONLY" = compat ] || run def $DEFREF
exit $rc
