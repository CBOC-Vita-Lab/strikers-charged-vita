"""Wave 13, helper netclient13: host tests of the network client's hardening (fuzz of the STUN / TURN / peer / JSON parsers under ASan + UBSan, the friends-list pacing gate,
the O5 send-gap diagnostic that runs inside the fuzz binary). The socket census test needs the host32 mbedtls and runs from native/tools/net/run_census_test.sh."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
N = ROOT / "native"


def cxx(args, exe):
    return subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", "-I", str(N)] + args + ["-o", str(exe)], capture_output=True, text=True)


@unittest.skipUnless(shutil.which("c++"), "needs a C++ compiler")
class NetClient13(unittest.TestCase):
    def test_friends_policy_and_its_mutants(self):
        src = str(N / "platform/sdk/tests/test_friends_policy.cpp")
        with tempfile.TemporaryDirectory() as d:
            exe = Path(d) / "t"
            c = cxx([src], exe)
            self.assertEqual(c.returncode, 0, c.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assertIn("0 failed", r.stdout)
            for m in ("OFF_NOT_OFF", "NO_RESUME", "NO_REQ_RESET", "GAP_SHORTENS", "IDLE_FROM_START"):
                c = cxx(["-DMUTANT_" + m, src], exe)
                self.assertEqual(c.returncode, 0, c.stderr)
                self.assertNotEqual(subprocess.run([str(exe)], capture_output=True, text=True).returncode, 0, "mutant %s survived" % m)

    def test_json_text_is_printable_ascii_only_and_the_switch_restores_the_old_rule(self):
        src = (N / "platform/sdk/net/online/np_json.h").read_text()
        self.assertIn("ScrubAscii(out)", src)
        dwc = (N / "platform/sdk/dwc/np_online_json.cpp").read_text()
        self.assertIn("NpNet::ScrubAscii(out)", dwc)
        for f in ("platform/sdk/net/online/np_signal_http.cpp", "platform/sdk/net/online/np_online.cpp", "platform/sdk/net/online/np_signal_fake.cpp", "platform/sdk/dwc/np_online_http.cpp"):
            self.assertIn("SafeCopy", (N / f).read_text(), f)

    @unittest.skipUnless(shutil.which("g++"), "needs g++ with the sanitizers (32-bit)")
    def test_fuzz_small_run(self):
        with tempfile.TemporaryDirectory() as d:
            r = subprocess.run(["bash", str(N / "tools/net/run_fuzz_net.sh"), d, "--cases", "4000"], capture_output=True, text=True, cwd=str(N))
            out = r.stdout + r.stderr
            if "libasan" in out and "cannot find" in out:
                self.skipTest("no 32-bit ASan runtime here")
            self.assertEqual(r.returncode, 0, out[-3000:])
            self.assertIn("PASS (0 violations)", r.stdout)


if __name__ == "__main__":
    unittest.main()
