"""Builds and runs the host test of MSC_HDTEX_FACES (renderer/tests/hdtex_faces.cpp): the blink's eye frames in an HD pack stay the game's own by default."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent


class HdTexFaces(unittest.TestCase):
    @unittest.skipUnless(shutil.which("g++"), "no host compiler")
    def test_eye_frames(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = pathlib.Path(tmp) / "hdtex_faces"
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-fsanitize=address,undefined", "-pthread", "-I", str(ROOT / "renderer/src"), "-I", str(ROOT / "renderer/third_party"),
                            str(ROOT / "renderer/tests/hdtex_faces.cpp"), str(ROOT / "renderer/src/vgx_hdtex.cpp"), "-o", str(exe)], check=True)
            for value, eyes in ((None, "0"), ("0", "0"), ("1", "1")):
                with self.subTest(MSC_HDTEX_FACES=value):
                    env = {k: v for k, v in os.environ.items() if not k.startswith("MSC_HDTEX")}
                    if value is not None:
                        env["MSC_HDTEX_FACES"] = value
                    result = subprocess.run([str(exe), str(pathlib.Path(tmp) / "t.pack"), eyes], capture_output=True, text=True, env=env)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
