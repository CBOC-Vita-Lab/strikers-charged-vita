"""MSC_RAIN_HULL (np/vgx_rainhull.h): the host test in native/tools/rainhull, and its live mutant."""
import os
import shutil
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class RainHull(unittest.TestCase):
    @unittest.skipUnless(shutil.which("g++"), "no g++")
    def test_cover(self):
        r = subprocess.run(["sh", os.path.join(ROOT, "native/tools/rainhull/run.sh")], capture_output=True, text=True, timeout=300)
        self.assertEqual(r.returncode, 0, r.stdout[-2000:] + r.stderr[-2000:])

    @unittest.skipUnless(shutil.which("g++"), "no g++")
    def test_a_rectangle_that_is_too_small_is_caught(self):
        r = subprocess.run(["sh", os.path.join(ROOT, "native/tools/rainhull/run.sh")], capture_output=True, text=True, timeout=300, env=dict(os.environ, RAINHULL_BREAK="1"))
        self.assertNotEqual(r.returncode, 0)

    def test_hull_keeps_no_frame_memory(self):
        """The crash of data16 (signal 11 at 0xcdcdcdf1 in glx_BindTexture): the hull block kept a copy of a packet whose material parameters were glFrameAlloc memory of the build
        picture. The block must hold geometry only and every drop must take a fresh copy of the source frame model."""
        src = open(os.path.join(ROOT, "native/src/Game/Effects/ParticleSystem.cpp")).read()
        a, b = src.index("bool RainBuild("), src.index("static TweakValueFloat sfParticleRedScale")
        build = src[a:b]
        self.assertNotIn("*hp = k", build)
        self.assertNotIn("materialParameters", build)
        self.assertNotIn("*hm = *m", build)
        self.assertNotIn("glModelDupNoStreams(rh->cls", src)
        self.assertIn("pModel = pAnim->GetModel(rainFi);", src)


if __name__ == "__main__":
    unittest.main()
