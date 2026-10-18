#!/bin/bash
# Makes the (weak) import stubs of kubridge from its export description: no prebuilt package is
# installed in this workspace. Output: <outdir>/libkubridge_stub.a and libkubridge_stub_weak.a.
# The native port links the WEAK one: without kubridge.skprx on the console the imports are null
# and the program still starts (np_vita_crash.cpp tests the function's address).
#   gen_kubridge_stubs.sh <outdir>
set -e
OUT=${1:?outdir}
KUBRIDGE=${KUBRIDGE:-/home/claude/research/vita-refs/bythos14_kubridge}
VBIN=${VBIN:-/opt/vitasdk/bin}
rm -rf "$OUT" && mkdir -p "$OUT/gen"
# vita-elf-export turns the export list into an import database; its "elf" argument is only hashed
# to make up a module NID, which import stubs do not contain (they carry the library NID, derived
# from the library name, and the function NIDs), so the description file itself stands in for it.
$VBIN/vita-elf-export kernel "$KUBRIDGE/exports.yml" "$KUBRIDGE/exports.yml" "$OUT/kubridge_imports.yml"
$VBIN/vita-libs-gen "$OUT/kubridge_imports.yml" "$OUT/gen"
make --no-print-directory -C "$OUT/gen" > "$OUT/gen.log" 2>&1
cp "$OUT/gen/libkubridge_stub.a" "$OUT/gen/libkubridge_stub_weak.a" "$OUT/"
ls -l "$OUT"/*.a
