"""Builds and runs the host test of np/scenecut.h (the policy of the 1.3 scene cuts: MSC_FX_GLOW, MSC_SHADOW_EVERY, MSC_REPLAY_FARLOD, MSC_CROWD_REPLAY, MSC_CROWD_CLUSTER_LAZY, MSC_MENU_LOD, MSC_STADIUM_HEAVY)."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent


class SceneCut(unittest.TestCase):
    @unittest.skipUnless(shutil.which("g++"), "no host compiler")
    def test_policy(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = pathlib.Path(tmp) / "scenecut_test"
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-I", str(ROOT / "native/include"),
                            str(ROOT / "native/tools/scenecut/scenecut_test.cpp"), "-o", str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == "__main__":
    unittest.main()
