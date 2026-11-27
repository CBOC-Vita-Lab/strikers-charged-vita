#!/bin/bash
# tools/seedtest/run_migrate.sh: PC test of the cache-layout code of vita/vgx_host_vita.cpp (cache version and file names, the older key, the one-time move of a
# cache an earlier build left). The blocks are cut out of that file by their first and last lines, as run.sh does for the seed copy.
set -e
T=$(cd "$(dirname "$0")" && pwd); F=$T/../../vita/vgx_host_vita.cpp
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT; cd $W
cut_() { local a b; a=$(grep -n "$1" $F | head -1 | cut -d: -f1); b=$(grep -n "$2" $F | head -1 | cut -d: -f1); [ -n "$a" ] && [ -n "$b" ] || { echo "marker missing: $1 / $2"; exit 2; }; sed -n "${a},$((b-1))p" $F > $3; }
cut_ '^struct SeedCopyStats {' '^// `fresh`: the destination folder' part_seedcopy.inc
cut_ "^// vitaGL's cache layout, <cache>/v<version>" '^// ux0:data/mscharged/shadercache/<8 hex digits>' part_version.inc
cut_ '^uint32_t CacheKeyOf' '^const char\* ShaderCacheFolder() {' part_keys.inc
cut_ '^// ---- the shaders an older build kept' '^// Returns the folder vitaGL is to use as its cache.' part_move.inc
cp $T/shim.h $T/test_migrate.cpp .
sed -i 's/^namespace {$/namespace {/' test_migrate.cpp
g++ -std=c++20 -O1 -w -I/work/msc/deps/xxHash -o tm test_migrate.cpp
mkdir -p root; ./tm $W/root
