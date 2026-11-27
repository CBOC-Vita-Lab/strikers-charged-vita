"""Save-data safety (12 Nov 2026): the cut tests of the file replace / recovery code, through the real NAND layer, and the one-time save import."""
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
SDK = ROOT / "native/platform/sdk"


@unittest.skipUnless(shutil.which("g++"), "needs g++")
class SaveSafety(unittest.TestCase):
    def run_script(self, script):
        r = subprocess.run([sys.executable, str(SDK / "tests" / script)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-2000:])
        return r.stdout

    def test_safe_file_cut_matrix(self):
        out = self.run_script("run_safe_file_tests.py")
        self.assertIn("0 failed", out)

    def test_game_save_cut_matrix_through_nand_native(self):
        out = self.run_script("run_nand_save_cut_test.py")
        self.assertIn("0 failed", out)

    def test_save_import_is_once(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = pathlib.Path(tmp) / "test_save_import"
            native = ROOT / "native"
            cmd = ["g++", "-m32", "-std=gnu++17", "-O1", "-w", "-fshort-wchar", "-funsigned-char", "-fno-short-enums", "-DNP_NATIVE=1", "-DNP_HOST=1",
                   f"-I{SDK}", f"-I{native}", f"-I{native / 'include'}", f"-I{SDK / 'include'}", f"-I{native / 'libs/RVL_SDK/include'}", "-include", "np/compat.h",
                   str(SDK / "tests/test_save_import.cpp"), "-o", str(exe)]
            if subprocess.run(cmd, capture_output=True).returncode != 0:
                cmd.remove("-m32")
                subprocess.run(cmd, check=True)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout[-2000:])
            self.assertIn("0 failed", r.stdout)


if __name__ == "__main__":
    unittest.main()
