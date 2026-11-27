"""MSC_FX_TIGHT and MSC_FX_GOV: the host tests in native/tools/fxtight and renderer/tests."""
import os
import shutil
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class FxTight(unittest.TestCase):
    @unittest.skipUnless(shutil.which("g++"), "no g++")
    def test_polygons(self):
        r = subprocess.run(["sh", os.path.join(ROOT, "native/tools/fxtight/run.sh")], capture_output=True, text=True, timeout=600)
        self.assertEqual(r.returncode, 0, r.stdout[-2000:] + r.stderr[-2000:])

    @unittest.skipUnless(shutil.which("g++"), "no g++")
    def test_a_broken_polygon_is_caught(self):
        env = dict(os.environ, FXTIGHT_BREAK="1")
        r = subprocess.run(["sh", os.path.join(ROOT, "native/tools/fxtight/run.sh")], capture_output=True, text=True, timeout=600, env=env)
        self.assertNotEqual(r.returncode, 0)

    @unittest.skipUnless(shutil.which("g++"), "no g++")
    def test_governor(self):
        env = dict(os.environ, CXX="g++")
        r = subprocess.run(["sh", os.path.join(ROOT, "renderer/tests/run_fxgov_test.sh")], capture_output=True, text=True, timeout=300, env=env)
        self.assertEqual(r.returncode, 0, r.stdout[-2000:] + r.stderr[-2000:])


if __name__ == "__main__":
    unittest.main()
