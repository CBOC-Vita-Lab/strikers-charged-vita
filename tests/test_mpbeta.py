"""mpbeta27: multiplayer is a public beta. The session trail of a problem report (mode, stage, errors, requests, sockets, lockstep health, the partner's build), the offer
rules (a dropped ranked result, a crash inside a session in the last run), the scrub of everything the trail adds, the BETA labels and their texts in three languages.

Each mutant must be CAUGHT: a scrubber that does nothing, trail lines that are not scrubbed, a dropped ranked result that does not offer a report, a crash marker that is
never removed (every start would "find a crash"), and labels without the tag."""
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
TSV = N / "platform/sdk/include/np/np_text_strings.tsv"
SOURCES = ["np_mpdiag.cpp", "np_mpreport.cpp"]


def build_run(flags=()):
    with tempfile.TemporaryDirectory() as d:
        exe = Path(d) / "t"
        c = subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-I", str(N), *flags, str(TESTS / "test_mpbeta.cpp"), *[str(NET / s) for s in SOURCES], "-pthread", "-o", str(exe)],
                           capture_output=True, text=True)
        if c.returncode:
            return c
        return subprocess.run([str(exe)], capture_output=True, text=True)


def table():
    rows = {}
    limit = None
    for line in TSV.read_text(encoding="utf-8").splitlines():
        if line.startswith("#@limit"):
            limit = int(line.split()[1])
            continue
        if not line.strip() or line.startswith("#"):
            continue
        en, es, fr = line.split("\t")
        rows[en] = (es, fr, limit)
    return rows


def sentences(text):
    # np_text.h Sentences: a sentence ends at . ! ? followed by a space or the end
    out, cur = [], ""
    i = 0
    while i < len(text):
        cur += text[i]
        if text[i] in ".!?" and (i + 1 == len(text) or text[i + 1] == " "):
            out.append(cur.strip())
            cur = ""
        i += 1
    if cur.strip():
        out.append(cur.strip())
    return out


def header_strings(name):
    src = (NET / "np_mpbeta.h").read_text()
    m = re.search(r"inline const char\* " + name + r"\([^)]*\) \{\s*return ([^;]+);", src)
    assert m, name
    return [s for s in re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))]


@unittest.skipUnless(shutil.which("c++"), "needs a C++ compiler")
class MpBeta(unittest.TestCase):
    def test_trail_offer_marker_scrub_and_labels(self):
        r = build_run()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertRegex(r.stdout, r"mpbeta: \d+ checks, 0 failed")

    def _caught(self, flag, why):
        r = build_run([flag])
        self.assertNotEqual(r.returncode, 0, "the tests did not notice: " + why + "\n" + r.stdout + r.stderr)
        self.assertIn("FAIL", r.stdout, r.stderr)

    def test_mutant_scrubber_off(self):
        self._caught("-DNP_MPDIAG_SCRUB_OFF", "a scrubber that does nothing")

    def test_mutant_trail_not_scrubbed(self):
        self._caught("-DNP_MPDIAG_MUT_TRAIL_NOSCRUB", "the trail's lines reach the report unscrubbed")

    def test_mutant_dropped_result_no_offer(self):
        self._caught("-DNP_MPDIAG_MUT_UPLOAD_NO_OFFER", "a ranked result lost for good offers no report")

    def test_mutant_marker_kept(self):
        self._caught("-DNP_MPDIAG_MUT_MARK_KEEP", "the crash marker is never removed")


class MpBetaSources(unittest.TestCase):
    def test_menu_labels_have_no_redundant_beta_tag(self):
        beta = (NET / "np_mpbeta.h").read_text()
        self.assertNotIn("ONLINE BETA", beta)
        header = (NET / "np_lan_text.h").read_text()
        self.assertNotIn("MULTIPLAYER BETA", header)
        self.assertNotIn("ONLINE CENTRAL BETA", header)

    def test_report_entry_and_notice_in_the_menu(self):
        flow = (NET / "np_menu_flow.cpp").read_text()
        self.assertIn("entry ? NpMpBeta::ReportButton() : \"CANCEL\"", flow)
        self.assertIn("r == 2 && NpMpBeta::ReportEntryOn()", flow)
        self.assertIn("case S_BETA_NOTICE:", flow)
        self.assertEqual(flow.count("BetaNoticeSeen();"), 1)
        # the report still starts only after SEND on the consent popup (np_mpreport.h): REPORT A PROBLEM goes through the same Ask
        self.assertEqual(flow.count("g_report.Start()"), 1)
        self.assertIn("void ReportPressed()", flow)

    def test_sentences_have_translations(self):
        rows = table()
        beta = (NET / "np_mpbeta.h").read_text()
        self.assertIn("REPORT A PROBLEM", rows)
        for name in ("NoticeText", "ResultLine", "SyncErrorText", "OtherBuildText", "OfferTail", "SavedInstead", "NotSaved"):
            text = "".join(header_strings(name))
            for s in sentences(text):
                self.assertIn(s, rows, f"{name}: {s!r} has no Spanish/French row")
        for lead in re.findall(r'"([^"]+ended with a problem\.|The game closed[^"]+)"', beta):
            self.assertIn(lead, rows, lead)

    def test_nothing_new_on_the_wire_or_in_the_match(self):
        # the trail is fed on events; the mismatch hook only logs; the crash marker is written at the lobby (LAN mode on), never in the match
        lan = (NET / "np_lan.cpp").read_text()
        self.assertEqual(lan.count("NpMpDiag::SessionMarkWrite("), 1)
        self.assertEqual(lan.count("NpMpDiag::SessionMarkClear("), 1)
        step = lan[lan.index("void NpLanStep(bool starved)"):]
        step = step[:step.index("\n}\n")]
        self.assertNotIn("SessionMark", step)
        self.assertNotIn("Reach(", step)
        router = (N / "src/Game/InputRouter.cpp").read_text()
        self.assertEqual(router.count("NpLanNoteMismatch("), 3)
        hook = lan[lan.index("void NpLanNoteMismatch("):]
        hook = hook[:hook.index("\n}\n")]
        for word in ("Send", "Write(", "mCurrentCRC", "mNetworkCRCs", "Disconnect", "PopupNetworkError"):
            self.assertNotIn(word, hook)
        for f in ("so_net.cpp", "vlink.cpp"):
            self.assertNotIn("mpbeta27", (NET / f).read_text(), f"{f}: the link and its fingerprint are unchanged")

    def test_switches_are_documented(self):
        tech = (ROOT / "docs/TECHNICAL.md").read_text()
        audit = (N / "platform/vita/tests/audit_settings_keys.py").read_text()
        for key in ("MSC_MP_BETA", "MSC_MP_BETA_NOTICE", "MSC_MP_REPORT_ENTRY", "MSC_MP_REPORT_PLUS", "MSC_MP_CRASH_MARK", "MSC_ONLINE_BUILD_HEADER"):
            self.assertIn(f"`{key}`", tech, key)
            self.assertIn(f'"{key}"', audit, key)


if __name__ == "__main__":
    unittest.main()
