#!/bin/bash
# Tests ordinary host32 prefix archives; never builds or fetches Mbed TLS.
# Usage: tools/https/run_tls13_transform_test.sh PREFIX SOURCE NEW_OUT
set -euo pipefail
if [ "$#" -ne 3 ]; then
  printf 'usage: %s PREFIX SOURCE NEW_OUT\n' "$0" >&2
  exit 2
fi
HERE=$(cd "$(dirname "$0")" && pwd -P)
PREFIX=$(cd "$1" && pwd -P)
SOURCE=$(cd "$2" && pwd -P)
OUT=$3
for input in "$PREFIX/lib/libmbedtls.a" "$PREFIX/lib/libmbedx509.a" "$PREFIX/lib/libmbedcrypto.a" \
             "$PREFIX/include/mbedtls/build_info.h" "$PREFIX/include/mbedtls/np_user_config.h" \
             "$SOURCE/library/ssl_misc.h" "$SOURCE/library/ssl_tls13_keys.h"; do
  if [ ! -f "$input" ] || [ ! -r "$input" ]; then
    printf 'missing required prefix/header input: %s\n' "$input" >&2
    exit 2
  fi
done
command -v gcc >/dev/null || { printf 'gcc is required\n' >&2; exit 2; }
command -v timeout >/dev/null || { printf 'timeout is required\n' >&2; exit 2; }
if [ -e "$OUT" ] || [ -L "$OUT" ]; then
  printf 'output must be a new path: %s\n' "$OUT" >&2
  exit 2
fi
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd -P)
FLAGS=(-m32 -msse2 -mfpmath=sse -idirafter /usr/include/x86_64-linux-gnu
  -funsigned-char -fno-short-enums -fno-strict-aliasing -fwrapv -fno-common
  -ffunction-sections -fdata-sections -O2 -g0 -std=gnu99 -Wall -Wextra
  -Wno-unused-function -I"$PREFIX/include" -I"$SOURCE/library")
if ! gcc "${FLAGS[@]}" "$HERE/tls13_transform_alloc_test.c" \
    "$PREFIX/lib/libmbedtls.a" "$PREFIX/lib/libmbedx509.a" "$PREFIX/lib/libmbedcrypto.a" \
    -Wl,--wrap=calloc,--wrap=free,--gc-sections -o "$OUT/tls13_transform_alloc_test" \
    > "$OUT/build.log" 2>&1; then
  head -40 "$OUT/build.log" >&2
  exit 1
fi
set +e
timeout 15s "$OUT/tls13_transform_alloc_test" > "$OUT/run.log" 2>&1
status=$?
set -e
cat "$OUT/run.log"
exit "$status"
