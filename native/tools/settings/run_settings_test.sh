#!/bin/bash
# Settings screen: every key a choice writes has a reader that accepts the value (audit), plus the settings runtime cases.
# Run from anywhere; needs a host g++.   Optional: BIN=<host harness> RUN_EFFECTIVE=1 also checks the [run] header per choice.
set -e
root=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$root"
python3 native/platform/vita/tests/audit_settings_keys.py
python3 native/platform/vita/tests/run_settings_runtime_tests.py
python3 native/platform/vita/tests/run_settings_display_tests.py
if [ -n "$RUN_EFFECTIVE" ] && [ -n "$BIN" ]; then python3 native/platform/vita/tests/check_settings_effective.py; fi
