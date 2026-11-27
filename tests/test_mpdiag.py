"""Wave 11 mpdiag18: the always-on multiplayer diagnostics (ring, scrubber, report) and the deeper Connection Test (classifier with fakes), host unit tests.

Each mutant must be CAUGHT: a build with the scrubber off, and one with the "choppy" delay class moved from 220 ms to 1000 ms, must fail the tests that pass on the real code."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
N = ROOT / "native"
NET = N / "platform/sdk/net"
TESTS = N / "platform/sdk/tests"


def build_run(test, sources, flags=()):
    with tempfile.TemporaryDirectory() as d:
        exe = Path(d) / "t"
        c = subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-I", str(N), *flags, str(TESTS / test), *[str(NET / s) for s in sources], "-pthread", "-o", str(exe)],
                           capture_output=True, text=True)
        if c.returncode:
            return c
        return subprocess.run([str(exe)], capture_output=True, text=True)


@unittest.skipUnless(shutil.which("c++"), "needs a C++ compiler")
class MpDiag(unittest.TestCase):
    def test_scrubber_ring_and_report(self):
        r = build_run("test_mpdiag.cpp", ["np_mpdiag.cpp"])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertRegex(r.stdout, r"mpdiag: \d+ checks, 0 failed")

    def test_mutant_scrubber_off_is_caught(self):
        r = build_run("test_mpdiag.cpp", ["np_mpdiag.cpp"], ["-DNP_MPDIAG_SCRUB_OFF"])
        self.assertNotEqual(r.returncode, 0, "the tests did not notice a scrubber that does nothing")
        self.assertIn("leaked in", r.stdout)

    def test_connection_test_classifier_with_fakes(self):
        r = build_run("test_conntest.cpp", ["np_conntest.cpp"])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertRegex(r.stdout, r"conntest: \d+ checks, 0 failed")

    def test_mutant_choppy_class_is_caught(self):
        r = build_run("test_conntest.cpp", ["np_conntest.cpp"], ["-DNP_CONNTEST_CHOPPY_MS=1000"])
        self.assertNotEqual(r.returncode, 0, "the tests did not notice a moved delay class")

    def test_who_can_i_play_with_and_its_mutants(self):
        r = build_run("test_conntest.cpp", ["np_conntest.cpp"])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        src = (NET / "np_conntest.cpp").read_text()
        mutants = [
            ("    if (r[kWifi] == kWifiNone) return kPNoWifi;\n", "", "no Wi-Fi still offers the internet"),
            ("        if (r[kWifi] == kWifiNone) return kPRoomNearbyOnly;\n", "", "the room needs Wi-Fi"),
            ("    case kNetOk: return kPNone;", "    case kNetDown: return kPNone;", "a service that is down is a yes"),
            ("    if (who == kWhoFar) return r[kHosting] == kHostRelay || r[kWifi] == kWifiWeak ? kPCodeLag : kPCodeYes;", "    if (who == kWhoFar) return kPCodeYes;", "a relay is not mentioned for a far friend"),
            ("    const bool lag = r[kWifi] == kWifiWeak || r[kHosting] == kHostRelay || r[kDelay] == kDelayChoppy;", "    const bool lag = false;", "anyone online never lags"),
            ("        return r[kLocal] == kLocalBlocked ? kPRoomBlocked : kPRoomYes;", "        return kPRoomYes;", "a blocking Wi-Fi is not said"),
        ]
        for old, new, what in mutants:
            self.assertIn(old, src, what)
            with tempfile.TemporaryDirectory() as d:
                copy = Path(d) / "np_conntest.cpp"
                copy.write_text(src.replace(old, new, 1))
                shutil.copy(NET / "np_conntest.h", Path(d) / "np_conntest.h")
                exe = Path(d) / "t"
                c = subprocess.run(["c++", "-std=c++17", "-I", str(N), str(TESTS / "test_conntest.cpp"), str(copy), "-o", str(exe)], capture_output=True, text=True)
                self.assertEqual(c.returncode, 0, c.stderr)
                run = subprocess.run([str(exe)], capture_output=True, text=True)
                self.assertNotEqual(run.returncode, 0, "not caught: " + what)
        flow = (NET / "np_menu_flow.cpp").read_text()
        self.assertIn('getenv("MSC_WHO_CAN_I_PLAY_WITH")', flow)
        self.assertIn('WhoCanIPlayWithOn() ? "WHO CAN I PLAY WITH?" : "TEST CONNECTION"', flow)

    def test_hooks_are_single_lines_in_the_places_that_matter(self):
        lan = (NET / "np_lan.cpp").read_text()
        for needle in ("NpMpDiag::NoteGameLog(format, ring)", "NpMpDiag::Note(NpMpDiag::kSessionStart", "NpMpDiag::Note(NpMpDiag::kLinkLost", "NpMpDiag::Note(NpMpDiag::kMatchEnd",
                       "NpMpDiag::Note(NpMpDiag::kSessionEnd", "NpMpDiag::Note(NpMpDiag::kPeers", "NpMpDiag::Note(NpMpDiag::kInputDelay"):
            self.assertIn(needle, lan)
        # the hooks sit before the quiet-log and session guards of NpLanLog: a release build (no log) still records
        body = lan[lan.index("void NpLanLog(const char* format, ...) {"):]
        self.assertLess(body.index("NoteGameLog"), body.index("if (!NpLanActive() || g_quietLog) return;"))
        flow = (NET / "np_menu_flow.cpp").read_text()
        self.assertIn("ConnectionTestResult(", flow)
        self.assertIn("NpMpDiag::SetLastConnTest(line)", flow)

    def test_the_report_row_is_one_action_row_run_by_title(self):
        settings = (N / "platform/vita/src/np_settings.cpp").read_text()
        self.assertIn('{"ONLINE", {"ONLINE NAME", "SEND A REPORT", "SAVE A REPORT", "DELETE MY ONLINE ACCOUNT"}}', settings)   # (set18: the pixel screen's ONLINE page, opened by VITA SETTINGS > ONLINE ACCOUNT)
        glue = (N / "platform/vita/src/np_vita_settings.cpp").read_text()
        self.assertIn('NpSettings::ActionTitle(row.option), "SAVE A REPORT") && NpMpDiag::SaveReport(nullptr', glue)

    def test_the_ring_allocates_nothing_and_writes_nothing_on_an_event(self):
        text = (NET / "np_mpdiag.cpp").read_text()
        body = text[text.index("bool Ring::Push("):text.index("namespace {\n\nRing g_ring;")]
        for banned in ("malloc", "new ", "fopen", "fprintf", "snprintf", "std::string"):
            self.assertNotIn(banned, body)
        note = text[text.index("void Note(Kind kind"):text.index("void NoteGameLog")]
        for banned in ("malloc", "new ", "fopen", "fprintf", "std::string"):
            self.assertNotIn(banned, note)


if __name__ == "__main__":
    unittest.main()
