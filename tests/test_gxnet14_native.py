"""Wave 14, helper gxnet14: host tests of the online ports from the Galaxy line (the registration proof-of-work solver against the service's reference nonces, the keyboard's
UTF-16 to UTF-8) and a check that the client switches exist. The flow tests need the service fake / the stub server and run from native/tools/net/run_online_tests.sh --held
and native/tools/dwc/run_dwc_http_test.sh."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
N = ROOT / "native"


@unittest.skipUnless(shutil.which("c++"), "needs a C++ compiler")
class GxNet14(unittest.TestCase):
    def test_standalone_host_tests(self):
        with tempfile.TemporaryDirectory() as d:
            r = subprocess.run(["bash", str(N / "tools/net/run_gxnet14_tests.sh"), d], capture_output=True, text=True, cwd=str(N))
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("pow_test: all passed", r.stdout)
            self.assertIn("ime_utf8_test: all passed", r.stdout)

    def test_switches_are_read_and_documented(self):
        doc = (ROOT / "docs/TECHNICAL.md").read_text()
        for sw, files in {
            "MSC_NET_POW": ["platform/sdk/dwc/np_online_http.cpp"],
            "MSC_NET_EDGE_BUSY": ["platform/sdk/net/online/np_service_busy.h"],
            "MSC_NET_PACE_HINTS": ["platform/sdk/net/online/np_service_busy.h"],
        }.items():
            self.assertIn("`%s`" % sw, doc, sw)
            for f in files:
                self.assertIn(sw, (N / f).read_text(), (sw, f))

    def test_keyboard_uses_the_shared_converter(self):
        src = (N / "platform/sdk/dwc/np_online_ime_vita.cpp").read_text()
        self.assertIn("NpUtf16ToUtf8", src)
        self.assertNotIn("0xE0 | (v >> 12)", src)  # the old inline 3-byte writer (surrogates became invalid UTF-8)


if __name__ == "__main__":
    unittest.main()
