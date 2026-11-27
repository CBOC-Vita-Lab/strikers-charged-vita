#!/bin/bash
# Real owning-method lifecycle test; no game assets, graphics context, or prebuilt objects.
# Linux x86 multilib + Clang-compatible compiler. Existing outputs are never overwritten.
# Usage: [CXX=/path/to/clang++] [NM=nm] [TIMEOUT=timeout] script [NEW_OUT]
# No argument/OUT creates an exclusive parent under TMPDIR (or /tmp), retains its run leaf,
# and prints the artifact path. Explicit OUT=NEW_OUT is also accepted. HOST_SYSTEM_INCLUDE overrides
# the optional Linux multiarch fallback header directory; set it empty for a toolchain sysroot.
set -euo pipefail
if [ "$#" -gt 1 ]; then
  printf 'usage: %s [NEW_OUT] (or OUT=NEW_OUT with no argument)\n' "$0" >&2
  exit 2
fi
if [ "$#" -eq 1 ] && [ -n "${OUT:-}" ] && [ "$OUT" != "$1" ]; then
  printf 'conflicting OUT and positional output paths\n' >&2
  exit 2
fi
AUTO_OUTPUT=0
if [ "$#" -eq 0 ] && [ -z "${OUT:-}" ]; then AUTO_OUTPUT=1; fi
OUT=${1:-${OUT:-}}
if [ "$AUTO_OUTPUT" = 0 ] && { [ -z "$OUT" ] || [ -e "$OUT" ] || [ -L "$OUT" ]; }; then
  printf 'output must be a new nonempty path: %s\n' "$OUT" >&2
  exit 2
fi
HERE=$(cd "$(dirname "$0")" && pwd -P)
ROOT=$(cd "$HERE/../../.." && pwd -P)
NATIVE="$ROOT/native"
CXX=${CXX:-clang++}
NM=${NM:-nm}
TIMEOUT=${TIMEOUT:-timeout}
for tool in "$CXX" "$NM" "$TIMEOUT" sha256sum cmp; do
  command -v "$tool" >/dev/null || { printf 'required tool not found: %s\n' "$tool" >&2; exit 2; }
done
if [ "$AUTO_OUTPUT" = 1 ]; then
  command -v mktemp >/dev/null || { printf 'mktemp is required for automatic output\n' >&2; exit 2; }
fi
FLAGS=(-m32 -msse2 -mfpmath=sse -std=gnu++11 -O1 -g0
  -funsigned-char -fno-short-enums -fno-strict-aliasing -fwrapv
  -fshort-wchar -fno-delete-null-pointer-checks -fno-common
  -fno-exceptions -fno-rtti -fdeclspec -ffunction-sections -fdata-sections
  -DNP_NATIVE=1 -DNP_HOST=1 -DNP_TARGET_HOST32=1 -DBUILD_VERSION=0
  -DVERSION_R4QE01 -DNDEBUG=1
  -I"$NATIVE/platform/sdk/shim" -I"$NATIVE/platform/sdk/shim/internal"
  -I"$NATIVE/include" -I"$NATIVE/platform/sdk/include" -I"$NATIVE/src/ode"
  -I"$NATIVE/libs/RVL_SDK/include" -I"$NATIVE/libs/Runtime/include"
  -idirafter "$NATIVE/libs/MSL_C/include" -idirafter "$NATIVE/libs/MetroTRK/include"
  -include np/compat.h)
if [ "${HOST_SYSTEM_INCLUDE+x}" = x ]; then
  SYSTEM_INCLUDE=$HOST_SYSTEM_INCLUDE
elif [ -d /usr/include/x86_64-linux-gnu ]; then
  SYSTEM_INCLUDE=/usr/include/x86_64-linux-gnu
else
  SYSTEM_INCLUDE=
fi
if [ -n "$SYSTEM_INCLUDE" ]; then
  test -d "$SYSTEM_INCLUDE" || { printf 'missing HOST_SYSTEM_INCLUDE: %s\n' "$SYSTEM_INCLUDE" >&2; exit 2; }
  FLAGS+=(-idirafter "$SYSTEM_INCLUDE")
fi
SOURCES=("$NATIVE/src/Game/Render/ImpostorSprite.cpp"
  "$NATIVE/src/Game/Render/ImpostorCharacter.cpp" "$NATIVE/src/Game/Render/ImpostorManager.cpp"
  "$NATIVE/src/NL/nlSlotPool.cpp" "$NATIVE/src/NL/nlRandom.cpp"
  "$NATIVE/src/Game/GL/GLCompactColourMeshWriter.cpp"
  "$HERE/capture_lifecycle_platform.cpp" "$HERE/capture_lifecycle_test.cpp")
for source in "${SOURCES[@]}"; do
  test -f "$source" && test -r "$source" || { printf 'missing source: %s\n' "$source" >&2; exit 2; }
done
if [ "$AUTO_OUTPUT" = 1 ]; then
  PARENT=$(mktemp -d "${TMPDIR:-/tmp}/capture-lifecycle.XXXXXXXX")
  OUT="$PARENT/run"
fi
mkdir -p "$(dirname "$OUT")"
mkdir "$OUT"
OUT=$(cd "$OUT" && pwd -P)
printf 'capture lifecycle artifacts: %s\n' "$OUT"
"$CXX" --version > "$OUT/compiler.txt"
sha256sum "${SOURCES[@]}" "$HERE/run_capture_lifecycle_test.sh" > "$OUT/source-before.sha256"
printf '%q ' "${FLAGS[@]}" > "$OUT/flags.txt"
printf '\n' >> "$OUT/flags.txt"
OBJECTS=()
index=0
for source in "${SOURCES[@]}"; do
  object="$OUT/$index.o"
  OBJECTS+=("$object")
  printf '%q ' "$CXX" "${FLAGS[@]}" -MMD -c "$source" -o "$object" >> "$OUT/commands.txt"
  printf '\n' >> "$OUT/commands.txt"
  if "$CXX" "${FLAGS[@]}" -MMD -c "$source" -o "$object" >> "$OUT/build.log" 2>&1; then
    :
  else
    status=$?
    printf '%s\n' "$status" > "$OUT/build-exit.txt"
    head -40 "$OUT/build.log" >&2
    exit "$status"
  fi
  index=$((index + 1))
done
BIN="$OUT/capture_lifecycle_test"
printf '%q ' "$CXX" -m32 -Wl,--gc-sections "${OBJECTS[@]}" -lm -o "$BIN" >> "$OUT/commands.txt"
printf '\n' >> "$OUT/commands.txt"
if "$CXX" -m32 -Wl,--gc-sections "${OBJECTS[@]}" -lm -o "$BIN" >> "$OUT/build.log" 2>&1; then
  printf '0\n' > "$OUT/build-exit.txt"
else
  status=$?
  printf '%s\n' "$status" > "$OUT/build-exit.txt"
  head -40 "$OUT/build.log" >&2
  exit "$status"
fi
"$NM" -C "$BIN" > "$OUT/linked-symbols.txt"
sha256sum "$OUT/"*.o "$OUT/"*.d "$BIN" > "$OUT/artifacts.sha256"
set +e
"$TIMEOUT" 15s "$BIN" > "$OUT/run.log" 2>&1
status=$?
set -e
printf '%s\n' "$status" > "$OUT/exit.txt"
sha256sum "${SOURCES[@]}" "$HERE/run_capture_lifecycle_test.sh" > "$OUT/source-after.sha256"
if ! cmp "$OUT/source-before.sha256" "$OUT/source-after.sha256"; then
  printf 'source or runner changed during the test\n' >&2
  exit 2
fi
cat "$OUT/run.log"
exit "$status"
