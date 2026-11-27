#!/usr/bin/env python3
"""Host test of the stale-settings guard against real 1.0 / 1.1 np-player.txt files (tests/fixtures/np-player), with the address and undefined-behaviour sanitizers."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def main():
    vita = Path(__file__).resolve().parents[1]
    command = shlex.split(os.environ.get("CXX", "c++")) + ["-std=c++17", "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I" + str(vita / "src")]
    with tempfile.TemporaryDirectory(prefix="settings-guard-") as temporary:
        binary = Path(temporary) / "test_settings_guard"
        subprocess.run(command + [str(vita / "tests/test_settings_guard.cpp"), str(vita / "src/np_settings_guard.cpp"), str(vita / "src/np_settings.cpp"), "-o", str(binary)], check=True)
        kit = Path(__file__).resolve().parents[4] / "tools/console-1.2/env"   # the console test kit's env files must pass the guard untouched
        result = subprocess.run([str(binary), str(vita / "tests/fixtures/np-player")] + ([str(kit)] if kit.is_dir() else []))
    sys.exit(result.returncode)


if __name__ == "__main__":
    main()
