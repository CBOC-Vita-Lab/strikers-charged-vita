"""SEND A REPORT / DELETE MY ONLINE ACCOUNT (client): the payload builder, the consent rule, the offer-once rule and the answers, host unit tests.

Each mutant must be CAUGHT: a scrubber that does nothing, a size limit that is not applied, a consent step that can be skipped, a local identity that is forgotten
although the service did not delete the account, the daily limit told as a failure, "already gone" (401) told as a failure, and an offer that is made again and again."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
N = ROOT / "native"
NET = N / "platform/sdk/net"
TESTS = N / "platform/sdk/tests"
SOURCES = ["np_mpreport.cpp", "np_mpdiag.cpp"]


def build_run(flags=()):
    with tempfile.TemporaryDirectory() as d:
        exe = Path(d) / "t"
        c = subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-I", str(N), *flags, str(TESTS / "test_mpreport.cpp"), *[str(NET / s) for s in SOURCES], "-pthread", "-o", str(exe)],
                           capture_output=True, text=True)
        if c.returncode:
            return c
        return subprocess.run([str(exe)], capture_output=True, text=True)


@unittest.skipUnless(shutil.which("c++"), "needs a C++ compiler")
class MpReport(unittest.TestCase):
    def test_payload_consent_offer_and_answers(self):
        r = build_run()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertRegex(r.stdout, r"mpreport: \d+ checks, 0 failed")

    def _caught(self, flag, why):
        r = build_run([flag])
        self.assertNotEqual(r.returncode, 0, "the tests did not notice: " + why + "\n" + r.stdout + r.stderr)
        self.assertIn("FAIL", r.stdout, r.stderr)

    def test_mutant_scrubber_off(self):
        self._caught("-DNP_MPDIAG_SCRUB_OFF", "a scrubber that does nothing (addresses and ids reach the payload)")

    def test_mutant_no_size_cap(self):
        self._caught("-DNP_MPREPORT_MUT_NOCAP", "a payload over 48 KiB")

    def test_mutant_no_consent(self):
        self._caught("-DNP_MPREPORT_MUT_NOCONSENT", "a request started without SEND / YES")

    def test_mutant_forget_always(self):
        self._caught("-DNP_MPREPORT_MUT_FORGET_ALWAYS", "the identity forgotten although the account was not deleted")

    def test_mutant_limit_as_failure(self):
        self._caught("-DNP_MPREPORT_MUT_LIMIT_AS_FAIL", "the daily limit not told as the daily limit")

    def test_mutant_401_is_failure(self):
        self._caught("-DNP_MPREPORT_MUT_401_FAIL", "'already gone' told as a failure")

    def test_mutant_offer_always(self):
        self._caught("-DNP_MPDIAG_MUT_OFFER_ALWAYS", "the offer made again after it was made")

    def test_the_settings_glue_asks_before_it_sends(self):
        glue = (N / "platform/vita/src/np_vita_settings.cpp").read_text()
        # the press only asks (no network); the request starts after the dialog's SEND / YES: Consent, then Start, never one without the other
        self.assertIn("report.Ask(", glue)
        self.assertEqual(glue.count("report.Consent();"), 1)
        self.assertEqual(glue.count("report.Start()"), 1)
        self.assertRegex(glue, r"report\.Consent\(\);\s+if \(!report\.Start\(\)\)")
        # Circle / CANCEL / NO close it without starting
        self.assertIn("reportChoice = 0;", glue)

    def test_the_menu_offers_once_and_asks_before_it_sends(self):
        flow = (NET / "np_menu_flow.cpp").read_text()
        self.assertEqual(flow.count("MarkOffered()"), 1)
        self.assertEqual(flow.count("g_report.Start()"), 1)
        self.assertLess(flow.index("g_report.Consent()"), flow.index("g_report.Start()"))

    def test_no_online_call_unless_the_player_asks(self):
        # the report and the delete are reached only through Op (np_mpreport.cpp); nothing in the menu flow or the settings glue builds these URLs itself
        for f in (NET / "np_menu_flow.cpp", N / "platform/vita/src/np_vita_settings.cpp", NET / "np_lan.cpp"):
            text = f.read_text()
            self.assertNotIn("/v1/report", text, f.name)
            self.assertNotIn("/v1/account", text, f.name)


if __name__ == "__main__":
    unittest.main()
