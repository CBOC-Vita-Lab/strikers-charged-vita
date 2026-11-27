"""Builds and runs the host test of np/stadiumcut.h (MSC_STADIUM_CUT: the parse, the per-stadium table, the far-size rule)."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent


class StadiumCut(unittest.TestCase):
    @unittest.skipUnless(shutil.which("g++"), "no host compiler")
    def test_policy(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = pathlib.Path(tmp) / "stadiumcut_test"
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-I", str(ROOT / "native/include"),
                            str(ROOT / "native/tools/stadiumcut/stadiumcut_test.cpp"), "-o", str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == "__main__":
    unittest.main()
