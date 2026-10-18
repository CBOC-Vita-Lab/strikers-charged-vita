#!/bin/bash
# The renderer's run modes against the glue's two tests (host32, GL on llvmpipe): every combination must pass and the pictures of one
# test must be byte-identical in all modes. Run after `make test-exe game-test-exe` (docs/render.md, "Test").
#   platform/gx/test/matrix.sh [out dir]        (about 30 s; the tests draw 3 frames each)
# Modes: MSC_VGX_PLAY (default thread / immediate / frame), MSC_VGX_BACKEND_THREAD=1 (the back end on its own thread),
# MSC_VGX_DLCACHE=0, MSC_VGX_DECODE=0, MSC_VGX_FAST=0 (the display-list cache, the specialised vertex decoder and its fast
# kernels off), and the arena registered as guest RAM or not (--no-ram).
here=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-/home/claude/msc/build/np/NPR/gx}
RUN=${VGX:-/home/claude/msc/npw/vgx-np}/native/test/run32.sh
fails=0
for t in gx_native_test:test-gl gx_game_test:gt-gl; do
  exe=${t%%:*}; dir=$OUT/${t##*:}
  [ -x $dir/$exe ] || { echo "missing $dir/$exe (make test-exe game-test-exe)"; exit 2; }
  ref=
  for mode in "" "MSC_VGX_PLAY=thread" "MSC_VGX_PLAY=immediate" "MSC_VGX_PLAY=frame" "MSC_VGX_BACKEND_THREAD=1" "MSC_VGX_DLCACHE=0" "MSC_VGX_DECODE=0" "MSC_VGX_FAST=0"; do
    for ram in "" "--no-ram"; do
      png=$dir/matrix_$$.png
      if (cd $dir && env $mode timeout 120 $RUN ./$exe $ram --png $png > $dir/matrix_$$.log 2>&1); then r=PASS; else r=FAIL; fails=$((fails+1)); fi
      sum=$(md5sum < $png 2>/dev/null | cut -c1-12)
      [ -z "$ref" ] && ref=$sum
      same=same; [ "$sum" = "$ref" ] || { same=DIFFERENT; fails=$((fails+1)); }
      printf '%-16s %-26s %-9s %s  picture %s (%s)\n' $exe "${mode:-default}" "${ram:-registered}" $r $sum $same
    done
  done
  rm -f $dir/matrix_$$.png $dir/matrix_$$.log
done
# the render mode's EFB height: the game's PAL-style modes use 448; the renderer's render target must follow it, reported by the SDK
# layer (--report-efb) or only through the display-copy rectangle
dir=$OUT/gt-gl
for opt in "--efb-h 448" "--efb-h 448 --report-efb"; do
  if (cd $dir && MSC_VGX_LOG=1 timeout 120 $RUN ./gx_game_test $opt --png $dir/matrix_$$.png > $dir/matrix_$$.log 2>&1) && grep -q 'render target 640x448 for EFB 640x448' $dir/matrix_$$.log && grep -q 'picture 640x448' $dir/matrix_$$.log; then r=PASS; else r=FAIL; fails=$((fails+1)); fi
  printf '%-16s %-26s %-9s %s\n' gx_game_test "$opt" "" $r
done
rm -f $dir/matrix_$$.png $dir/matrix_$$.log
echo "matrix: $fails problem(s)"
exit $((fails ? 1 : 0))
