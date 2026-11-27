"""Pair analysis must not discard contradictory or incomplete match evidence."""
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "native/tools/mp"))
import native_pair


SYNC = ("[mp-lan] sync tick=100 m=0 crc=12345678 seed=87654321\n"
        "[mp-lan] sync tick=100 m=1 crc=12345678 seed=87654321\n")
END = "[mp-lan] end digest: score 1-0 m0@119=456789ab/12345678 m1@120=89abcdef/23456789\n"


class NativePairIntegrity(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.logs = [Path(self.temp.name) / ("peer%d.log" % index) for index in (0, 1)]

    def analyse(self, first, second=None, lobby_only=False, check_speed=False):
        for path, body in zip(self.logs, (first, first if second is None else second)):
            path.write_text("[mp-lan] LAN mode on\n" + body + "[script] f=99 exit\n")
        options = {"lobby_only": True, "end_markers": ["STATE done", "STATE done"]} if lobby_only else {}
        return native_pair.analyse(self.logs, [0, 0], "1" if lobby_only else None, check_speed=check_speed, **options)

    @staticmethod
    def net(steps, active=30.0, kind="net match"):
        return ("[mp-lan] %s: t=%.1f s active=%.1f s steps=%d starved=0 (0.0%%) sps=1.0 delay=2.00 [2-2] now=2 mult=1.00 qlocal=1.00 "
                "qremote=1.00 rtt=5 ms tx=10.0/s 40 B max 60 rx=10.0/s 40 B max 60 send=5 us max 9 us (10 sends) rxq=1 ring=0 drops=0 flush=0\n"
                % (kind, active + 1, active, steps))

    def test_slow_console_makes_the_run_invalid(self):
        slow = self.net(int(30 * 50 * 0.5))
        report = self.analyse(SYNC + slow, SYNC + self.net(30 * 50), check_speed=True)
        self.assertEqual(report["verdict"], "INVALID")
        self.assertFalse(report["passed"])
        self.assertEqual(len(report["invalid"]), 1)

    def test_full_speed_and_short_runs_are_not_invalid(self):
        self.assertTrue(self.analyse(SYNC + self.net(30 * 50), check_speed=True)["passed"])
        self.assertTrue(self.analyse(SYNC + self.net(10, active=4.0), check_speed=True)["passed"])   # too short to say anything
        self.assertTrue(self.analyse(SYNC, check_speed=True)["passed"])                               # no net lines at all
        self.assertTrue(self.analyse(SYNC + self.net(int(30 * 50 * 0.65)), check_speed=True)["passed"])

    def test_speed_is_not_checked_unless_asked(self):
        self.assertTrue(self.analyse(SYNC + self.net(30), check_speed=False)["passed"])

    def test_game_without_completed_end_still_passes(self):
        self.assertTrue(self.analyse(SYNC)["passed"])

    def test_matching_end_accepts_different_final_ticks_per_machine(self):
        self.assertTrue(self.analyse(SYNC + END)["passed"])

    def test_identical_sync_repetition_is_accepted(self):
        self.assertTrue(self.analyse(SYNC + SYNC.splitlines(keepends=True)[0], SYNC)["passed"])

    def test_conflicting_crc_cannot_be_hidden_by_matching_later_record(self):
        bad = "[mp-lan] sync tick=100 m=0 crc=abcdef12 seed=87654321\n"
        report = self.analyse(bad + SYNC, SYNC)
        self.assertFalse(report["passed"])
        self.assertEqual(len(report["sync_conflicting_duplicates"]), 1)
        self.assertFalse(report["sync_mismatch_between_logs"])

    def test_conflicting_seed_cannot_be_hidden_by_matching_later_record(self):
        bad = "[mp-lan] sync tick=100 m=0 crc=12345678 seed=abcdef12\n"
        self.assertFalse(self.analyse(bad + SYNC, SYNC)["passed"])

    def test_same_tick_in_a_new_match_is_not_a_duplicate(self):
        later = SYNC.replace("tick=100", "tick=200")
        next_match = SYNC.replace("12345678", "abcdef12").replace("87654321", "bcdef123")
        self.assertTrue(self.analyse(SYNC + later + next_match)["passed"])

    def test_completed_matches_can_reuse_same_tick_with_different_crc(self):
        first = SYNC.replace("tick=100", "tick=0")
        second = first.replace("12345678", "abcdef12").replace("87654321", "bcdef123")
        first_end = "[mp-lan] end digest: score 1-0 m0@0=12345678/87654321 m1@0=12345678/87654321\n"
        second_end = first_end.replace("12345678", "abcdef12").replace("87654321", "bcdef123")
        report = self.analyse(first + first_end + second + second_end)
        self.assertTrue(report["passed"], report)
        self.assertEqual(report["sync_ticks_compared"], 4)
        self.assertEqual([peer["matches_seen"] for peer in report["peers"]], [2, 2])

    def test_completed_end_and_tick_drop_count_only_one_new_match(self):
        first = SYNC.replace("tick=100", "tick=200")
        second = SYNC.replace("12345678", "abcdef12").replace("87654321", "bcdef123")
        report = self.analyse(first + END + second + END)
        self.assertTrue(report["passed"], report)
        self.assertEqual([peer["matches_seen"] for peer in report["peers"]], [2, 2])

    def test_extra_completed_end_on_either_peer_is_rejected(self):
        for first, second in ((SYNC + END + END, SYNC + END), (SYNC + END, SYNC + END + END)):
            with self.subTest(first_ends=first.count("end digest")):
                self.assertFalse(self.analyse(first, second)["passed"])

    def test_completed_end_on_only_one_peer_is_rejected(self):
        self.assertFalse(self.analyse(SYNC + END, SYNC)["passed"])

    def test_missing_end_key_on_one_peer_is_rejected(self):
        partial = "[mp-lan] end digest: score 1-0 m0@119=456789ab/12345678\n"
        self.assertFalse(self.analyse(SYNC + END, SYNC + partial)["passed"])

    def test_end_records_with_no_common_keys_are_rejected(self):
        first = "[mp-lan] end digest: score 1-0 m0@119=456789ab/12345678\n"
        second = "[mp-lan] end digest: score 1-0 m1@120=89abcdef/23456789\n"
        self.assertFalse(self.analyse(SYNC + first, SYNC + second)["passed"])

    def test_bilateral_omission_of_observed_machine_is_rejected(self):
        partial = "[mp-lan] end digest: score 1-0 m0@119=456789ab/12345678\n"
        self.assertFalse(self.analyse(SYNC + partial)["passed"])

    def test_empty_completed_digests_are_rejected(self):
        self.assertFalse(self.analyse(SYNC + "[mp-lan] end digest: score 1-0\n")["passed"])

    def test_final_tick_key_mismatch_is_rejected(self):
        self.assertFalse(self.analyse(SYNC + END, SYNC + END.replace("m1@120", "m1@121"))["passed"])

    def test_end_score_and_value_mismatches_remain_rejected(self):
        for changed in (END.replace("score 1-0", "score 0-1"), END.replace("89abcdef", "abcdef12")):
            with self.subTest(changed=changed):
                self.assertFalse(self.analyse(SYNC + END, SYNC + changed)["passed"])

    def test_malformed_completed_digest_is_rejected(self):
        for bad in (END.replace("456789ab/12345678", "not-a-digest"),
                    END.replace("m0@119", "m4@119"), END.rstrip() + " unexpected\n"):
            with self.subTest(bad=bad):
                self.assertFalse(self.analyse(SYNC + bad)["passed"])

    def test_contradictory_duplicate_end_entries_are_rejected(self):
        for extra in ("m0@119=abcdef12/12345678", "m0@118=456789ab/12345678"):
            with self.subTest(extra=extra):
                self.assertFalse(self.analyse(SYNC + END.rstrip() + " " + extra + "\n")["passed"])

    def test_identical_duplicate_end_entry_is_accepted(self):
        repeated = END.rstrip() + " m0@119=456789ab/12345678\n"
        self.assertTrue(self.analyse(SYNC + repeated)["passed"])

    def test_expected_machine_coverage_is_per_completed_match(self):
        first_sync = SYNC.splitlines(keepends=True)[0]
        first_end = "[mp-lan] end digest: score 1-0 m0@119=456789ab/12345678\n"
        self.assertTrue(self.analyse(first_sync + first_end + SYNC + END)["passed"])

    def test_lobby_only_does_not_acquire_a_match_requirement(self):
        lobby = "[mp-lan] connection lost\n[script] f=90 STATE done\n"
        self.assertTrue(self.analyse(lobby, lobby_only=True)["passed"])
        self.assertFalse(self.analyse(lobby)["passed"])

    def test_lobby_only_still_rejects_present_conflicting_evidence(self):
        lobby = "[mp-lan] connection lost\n[script] f=90 STATE done\n"
        bad = "[mp-lan] sync tick=100 m=0 crc=abcdef12 seed=87654321\n"
        self.assertFalse(self.analyse(lobby + bad + SYNC, lobby + SYNC, lobby_only=True)["passed"])


if __name__ == "__main__":
    unittest.main()
