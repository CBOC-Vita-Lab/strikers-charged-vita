#!/bin/bash
# MSC_VGX_TEX_COMPRESS (src/vgx_texcomp.h), no GL (seconds). BREAK=n (1..18): a deliberately broken rule is compiled in; the run must FAIL.
# Also: the corpus hash is the same at -O0 and -O2, and, when Galaxy's source is on this machine (GALAXY_TEXCOMP=<vgx_texcomp.cpp>), the encoder equals Galaxy's byte for byte.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd); OUT=${OUT:-/tmp/vgx-texcomp}; mkdir -p "$OUT"; rm -rf "$OUT/card"; mkdir -p "$OUT/card"
GAL=${GALAXY_TEXCOMP:-/home/claude/galaxy31/galaxy/platform/vgx/src/vgx_texcomp.cpp}
F=(-std=c++20 -Wall -Wextra -ffp-contract=off -I"$VGX/src"); [[ ${BREAK:-0} != 0 ]] && F+=(-DVGX_TEXCOMP_BREAK=${BREAK})
SRC=("$VGX/tests/texcomp.cpp")
if [[ -f $GAL && ${BREAK:-0} == 0 ]]; then
  # Galaxy's encoder section (its own text, between "the encoder" and "the switch" headings), wrapped to stand alone
  S=$(grep -n '^// =* the encoder' "$GAL" | head -1 | cut -d: -f1); E=$(grep -n '^// =* the switch and its settings' "$GAL" | head -1 | cut -d: -f1)
  { echo '#include <algorithm>'; echo '#include <cmath>'; echo '#include <cstdint>'; echo '#include <cstring>'; echo '#include <vector>'
    echo 'namespace refgal { namespace texcomp {'
    echo 'struct Quality { double psnrRgb = 99.0, psnrA = 99.0; uint32_t maxErrRgb = 0, maxErrA = 0; };'
    echo 'enum : uint8_t { kFmtNone = 0, kFmtDxt1 = 1, kFmtDxt5 = 3 };'
    echo 'struct Encoded { uint8_t format = kFmtNone; std::vector<std::vector<uint8_t>> levels; Quality q; uint8_t lo[4] = {0, 0, 0, 0}, hi[4] = {255, 255, 255, 255}; };'
    echo 'void Downsample(const uint8_t*, uint32_t, uint32_t, std::vector<uint8_t>&, uint32_t&, uint32_t&) {}'
    echo 'void EncodeColorBlock(const uint8_t rgba[64], uint8_t out[8]); void EncodeAlphaBlock(const uint8_t rgba[64], uint8_t out[8]);'
    echo 'void EncodeDxt1(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t* dst); void EncodeDxt5(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t* dst);'
    echo 'void DecodeDxt(const uint8_t*, uint32_t, uint32_t, bool, uint8_t*); Quality Measure(const uint8_t*, const uint8_t*, size_t);'
    sed -n "$((S+1)),$((E-1))p" "$GAL"
    echo '} }'
    echo 'extern "C" void ref_encode_color(const uint8_t* px, uint8_t* o) { refgal::texcomp::EncodeColorBlock(px, o); }'
    echo 'extern "C" void ref_encode_alpha(const uint8_t* px, uint8_t* o) { refgal::texcomp::EncodeAlphaBlock(px, o); }'
    echo 'extern "C" void ref_encode_dxt(const uint8_t* r, uint32_t w, uint32_t h, int d5, uint8_t* d) { if (d5) refgal::texcomp::EncodeDxt5(r, w, h, d); else refgal::texcomp::EncodeDxt1(r, w, h, d); }'
  } > "$OUT/refenc.cpp"
  F+=(-DWITH_REF); SRC+=("$OUT/refenc.cpp"); echo "texcomp: comparing with Galaxy's encoder ($GAL lines $((S+1))-$((E-1)))"
fi
${CXX:-g++} "${F[@]}" -O2 -fsanitize=address,undefined "${SRC[@]}" -o "$OUT/texcomp"
if [[ ${BREAK:-0} != 0 ]]; then if "$OUT/texcomp" "$OUT/card" >/dev/null 2>&1; then echo "texcomp: FAILED (mutant ${BREAK} passed)"; exit 1; fi; echo "texcomp: mutant ${BREAK} caught"; exit 0; fi
"$OUT/texcomp" "$OUT/card"
# the same bytes whatever the optimiser does (the cache is made on the console, but the tests and the eval tools must agree with it)
${CXX:-g++} "${F[@]}" -O0 "${SRC[@]}" -o "$OUT/texcomp0"
${CXX:-g++} "${F[@]}" -O3 "${SRC[@]}" -o "$OUT/texcomp3"
a=$("$OUT/texcomp0" "$OUT/card" golden); b=$("$OUT/texcomp3" "$OUT/card" golden)
[[ "$a" == "$b" ]] || { echo "texcomp: corpus hash differs between -O0 ($a) and -O3 ($b)"; exit 1; }
echo "texcomp: corpus $a at -O0 and -O3"
