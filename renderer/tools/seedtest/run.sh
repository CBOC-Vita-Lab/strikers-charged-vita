#!/bin/bash
# tools/seedtest/run.sh <seed dir> [<dir of generated GLSL> <copy-pair GLSL dir>]
# PC test of the Vita host's shader-seed code (vita/vgx_host_vita.cpp: SeedShaderCache, the
# lazily made bucket folders), which no PC build compiles: the two blocks of that file are cut
# out and built against POSIX stand-ins for the sceIo calls (shim.h).
#   1. first start with the seed in the package, second start, start after a new package:
#      files copied, folders made, file-system calls of each;
#   2. (with GLSL directories) a start without a seed, every shader handed to
#      vgxvita_glShaderSource: the folders made must be exactly the seed's (the seed was
#      written by vitaGL itself, so this checks the file names computed here against vitaGL's).
# The GLSL of a warm-up list: vita/tools/shadercheck's gen_shaders <list> <dir>.
# xh.c prints vitaGL's cache file name for GLSL files ("#version 100\n" + text, as the back end
# sends it): xh <files> | cut -d' ' -f1 names the seed files a list needs.
set -e
T=$(cd "$(dirname "$0")" && pwd); F=$T/../../vita/vgx_host_vita.cpp; SEED=${1:?seed dir}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT; cd $W
a=$(grep -n '^constexpr const char\* kSeedDir' $F | cut -d: -f1); b=$(grep -n '^// The warm-up list of the session' $F | cut -d: -f1)
c=$(grep -n "^// ---- vitaGL's cache folders, made when they are needed" $F | cut -d: -f1); d=$(grep -n '^extern "C" int msc_vgl_printf' $F | cut -d: -f1)
sed -n "${a},$((b-1))p" $F > part_seed.inc; sed -n "${c},$((d-1))p" $F > part_bucket.inc
cp $T/shim.h $T/test_copy.cpp $T/test_lazy.cpp .
key=$(basename $(ls -d $SEED/*/ | head -1))
DEF=-DFOLDER="\"ux0:data/mscharged/shadercache/$key\""
g++ -std=c++20 -O1 -w "$DEF" -I/home/claude/msc/deps/xxHash -o t1 test_copy.cpp
g++ -std=c++20 -O1 -w "$DEF" -I/home/claude/msc/deps/xxHash -o t2 test_lazy.cpp
mkdir -p r1/app0/shadercache r1/ux0/data/mscharged; cp -a $SEED/$key r1/app0/shadercache/; echo eboot > r1/app0/eboot.bin
./t1 $W/r1 again touch
echo "ux0 after the three starts: $(find r1/ux0 -name '*.gxp' | wc -l) files, $(find r1/ux0/data/mscharged/shadercache/$key -type d | wc -l) folders; seed: $(find $SEED -name '*.gxp' | wc -l) files"
diff -rq $SEED/$key/v2 r1/ux0/data/mscharged/shadercache/$key/v2 | grep -v "Only in .*ux0.*: 00" || echo "copied tree identical to the seed (plus v/00, f/00)"
if [ -n "$2" ]; then
  mkdir -p r2/app0 r2/ux0/data/mscharged; echo eboot > r2/app0/eboot.bin
  ./t2 $W/r2 $2/vs_*.glsl $2/fs_*.glsl ${3:+$3/vs_copy.glsl $3/fs_copy.glsl}
  (cd r2/ux0/data/mscharged/shadercache/$key/v2 && find . -mindepth 2 -type d | sort) > lazy.txt
  (cd $SEED/$key/v2 && find . -mindepth 2 -type d; echo ./v/00; echo ./f/00) | sort -u > seed.txt
  echo "bucket folders made on demand: $(wc -l < lazy.txt); the seed's (with v/00, f/00): $(wc -l < seed.txt); differences: $(diff lazy.txt seed.txt | grep -c '^[<>]')"
fi
