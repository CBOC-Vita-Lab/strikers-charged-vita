#!/bin/bash
# Builds the clang profile runtime for armhf (clang 18 here ships no libclang_rt.profile-arm*): compiler-rt's lib/profile compiled with
# the armhf flags of the harness build. Output: $OUT/libclang_rt.profile-armhf.a (default OUT=build/pgo-runtime).
#   tools/pgo/build_runtime.sh [outdir]      (needs network once for the compiler-rt source tarball, or CRT_TARBALL=<local file>)
set -e
OUT=$(realpath -m "${1:-${OUT:-build/pgo-runtime}}"); VER=${CRT_VERSION:-18.1.3}
mkdir -p "$OUT/obj"; cd "$OUT"
TB=${CRT_TARBALL:-$OUT/compiler-rt-$VER.src.tar.xz}
[ -f "$TB" ] || curl -sSL -o "$TB" "https://github.com/llvm/llvm-project/releases/download/llvmorg-$VER/compiler-rt-$VER.src.tar.xz"
tar xf "$TB" --wildcards '*/lib/profile/*' '*/include/profile/*' '*/include/sanitizer/*'
SRC=$OUT/compiler-rt-$VER.src
F="--target=arm-linux-gnueabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -O2 -fPIC -fno-stack-protector -I$SRC/include -I$SRC/lib/profile"
for f in InstrProfiling InstrProfilingBuffer InstrProfilingFile InstrProfilingInternal InstrProfilingMerge InstrProfilingMergeFile \
         InstrProfilingNameVar InstrProfilingPlatformLinux InstrProfilingUtil InstrProfilingValue InstrProfilingVersionVar InstrProfilingWriter; do
  clang $F -DCOMPILER_RT_HAS_UNAME=1 -c "$SRC/lib/profile/$f.c" -o "obj/$f.o"
done
clang++ $F -fno-exceptions -fno-rtti -c "$SRC/lib/profile/InstrProfilingRuntime.cpp" -o obj/InstrProfilingRuntime.o
rm -f libclang_rt.profile-armhf.a; arm-linux-gnueabihf-ar rcs libclang_rt.profile-armhf.a obj/*.o
echo "$OUT/libclang_rt.profile-armhf.a"
