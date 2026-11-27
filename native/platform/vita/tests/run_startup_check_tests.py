#!/usr/bin/env python3
"""Host test of the start-up messages (np_startup_check) and of their Spanish and French text."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def main():
    vita = Path(__file__).resolve().parents[1]
    command = shlex.split(os.environ.get("CXX", "c++")) + ["-std=c++17", "-O1", "-g", "-fsanitize=address,undefined", "-I" + str(vita / "src"), "-I" + str(vita / "src/../../sdk/include")]
    with tempfile.TemporaryDirectory(prefix="startup-check-") as temporary:
        binary = Path(temporary) / "test_startup_check"
        subprocess.run(command + [str(vita / "tests/test_startup_check.cpp"), str(vita / "src/np_startup_check.cpp"), "-o", str(binary)], check=True)
        sys.exit(subprocess.run([str(binary)]).returncode)


if __name__ == "__main__":
    main()
