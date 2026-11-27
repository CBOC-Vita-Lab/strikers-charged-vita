#!/bin/bash
# The player's own game art (1.2 branding): the generator's tests (made-up game files) and the Settings screen with and without the art.
#   Always: tests/test_brand_from_disc.py; the Settings screen without art is the plain screen, pixel for pixel (fallback).
#   With NP_DISC_DIR=<folder that holds files/> (a prepared game): the art loads from the disc and draws the list and the controls page.
set -e
root=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$root"
python3 -m unittest tests.test_brand_from_disc
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
vita=native/platform/vita
"${CXX:-c++}" -std=c++17 -O1 -I$vita/src $vita/tests/test_brand_art.cpp $vita/src/np_brand_art.cpp $vita/src/np_settings.cpp $vita/src/vita_plan.cpp -o "$tmp/tba"
"$tmp/tba" - "$tmp"
if [ -n "$NP_DISC_DIR" ]; then
    "$tmp/tba" "$NP_DISC_DIR" "$tmp"
fi
echo "brand test: ok"
