#!/bin/bash
# tools/fi_replay/build.sh [host|arm] : builds fi_replay (5 small sources; only what is out of date).
#   host -> build/fi/tool/fi_replay        (x86-64, GLES2 on Mesa llvmpipe: pictures)
#   arm  -> build/fi/tool/fi_replay_arm    (armhf, stub GL: instruction counts under qemu)
# FI_MAXCC=<n> compiles at most n files and stops (three at a time outside the heavy lock).
set -e
MSC=/home/claude/msc; V=$MSC/vgx; T=$MSC/build/fi/tool; ARCH=${1:-host}
SRCS="$V/src/vgx_interp.cpp $V/src/vgx_capture.cpp $V/src/vgx_backend_gl.cpp $V/tools/fi_replay/fi_replay.cpp"
COMMON="-std=c++20 -O2 -g -pipe -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -DTARGET_PC -DNOMINMAX -I$V/src -I$MSC/deps/xxHash -I$MSC/deps/stb"
if [ $ARCH = arm ]; then
  O=$T/obj-arm; OUT=$T/fi_replay_arm; CXX="clang++ --target=arm-linux-gnueabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -I$V/src/glstub"
  LINK="arm-linux-gnueabihf-g++ -no-pie"; LIBS="-lpthread"
else
  O=$T/obj-host; OUT=$T/fi_replay; CXX="clang++ -march=x86-64-v3"; SRCS="$SRCS $V/src/vgx_host_egl.cpp"
  LINK="clang++"; LIBS="-lEGL -lGLESv2 -lpthread"
fi
mkdir -p $O; n=0; objs=""
for s in $SRCS; do
  o=$O/$(basename ${s%.cpp}).o; objs="$objs $o"
  if [ ! -f $o ] || [ $s -nt $o ] || [ -n "$(find $V/src -maxdepth 1 -name '*.h' -newer $o | head -1)" ]; then
    if [ -n "$FI_MAXCC" ] && [ $n -ge $FI_MAXCC ]; then echo "stopped after $n compiles (FI_MAXCC): run again"; exit 3; fi
    echo CXX $(basename $s); nice -n 15 $CXX $COMMON -c $s -o $o; n=$((n+1))
  fi
done
echo link $OUT; $LINK -o $OUT $objs $LIBS
