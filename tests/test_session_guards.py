"""LAN / online sessions must keep the single-player pacing out (lockstep: both consoles must advance by the same steps).

Every function of np_fps.cpp that changes how far the game clock advances for a late frame, when a pad is read, or when a picture is started has to ask
NpLanActive() itself: code, not convention. A new gate that is added without the guard (or a guard that is removed) fails here. Functions that do not exist
yet in a tree are skipped (the back-pressure gate arrives with agent/pacing11)."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = (ROOT / "native/platform/compat/np_fps.cpp").read_text()

# (a regex that finds the definition line, why a session must not run it)
GUARDED = [
    (r'^void SlipFrame\(', "slip budget: advances the game clock by less than the frame took"),
    (r'^extern "C" float NpSlipDelta\(', "slip: the old stateless rule"),
    (r'^extern "C" void NpFpsPadLateWait\(', "late pad read (changes which step a press lands in)"),
    (r'^int NpFpsPresentSyncMode\(', "present hold (measure only in a session)"),
    (r'^static bool BackPressureGate\(', "back-pressure before the pad (wave 11)"),
]


def body(regex):
    for m in re.finditer(regex, SRC, re.M):
        i = SRC.find("{", m.end())
        semi = SRC.find(";", m.end())
        if i >= 0 and (semi < 0 or i < semi):   # a definition, not a forward declaration
            break
    else:
        return None
    depth, j = 0, i
    while True:
        depth += SRC[j] == "{"
        depth -= SRC[j] == "}"
        j += 1
        if depth == 0:
            return SRC[i:j]


class SessionGuards(unittest.TestCase):
    def test_each_pacing_gate_asks_whether_a_session_exists(self):
        found = 0
        for regex, why in GUARDED:
            b = body(regex)
            if b is None:
                continue
            found += 1
            self.assertIn("NpLanActive", b, "%s (%s) does not consult NpLanActive()" % (regex, why))
        self.assertGreaterEqual(found, 4)   # the guard list itself must not go vacuous

    def test_cadence_between_and_load_pace_are_guarded_in_the_frame_function(self):
        self.assertRegex(SRC, r"ci\.enabled = g\.replayMode == 0 && !\(&NpLanActive != 0 && NpLanActive\(\)\)")
        self.assertRegex(SRC, r"const bool lan = &NpLanActive != 0 && NpLanActive\(\);\s*const bool eligible = [^;]*&& !lan &&")
        self.assertRegex(SRC, r"g_loadFast = LoadPace\(taskState, &NpLanActive != 0 && NpLanActive\(\) != 0\)")

    def test_tap_carry_is_off_in_a_session(self):
        text = (ROOT / "native/src/Game/Task/FixedUpdateTask.cpp").read_text()
        self.assertRegex(text, r"NpVitaPadHoldTaps\([^;]*!\(NpLanActive && NpLanActive\(\)\)")

    def test_the_game_forces_the_ai_budget_off_for_network_games(self):
        text = (ROOT / "native/src/Game/Game.cpp").read_text()
        i = text.index("if (IsNetworkOrRecordedGame())")
        self.assertIn("SetScriptTimeBudget(-1.0f);", text[i:i + 200])
        self.assertIn("gAIProfilingClock = GetGameSimulationMilliseconds;", text[i:i + 300])

    def test_rules_that_change_the_simulation_are_in_the_link_fingerprint(self):
        text = (ROOT / "native/platform/sdk/net/so_net.cpp").read_text()
        self.assertIn("NpGameFix(NP_FIX_NK_BUG)", text)
        self.assertIn("NpRuleNoMegaChosen()", text)

    def test_every_presentation_function_that_runs_in_a_match_is_synchronized_or_runs_with_the_simulation_stopped(self):
        """The loading wait of a cut-scene is local (card speed) unless the function is in IsSynchronizedNisFunction (determ handshake: every console starts
        the scene on the same step). A function the match can start while the simulation steps must be in that list; the others (walk-out, replay, results)
        run with the simulation stopped. A new in-match function fails here until someone decides which it is."""
        src = (ROOT / "native/src/Game/Render/Presentation.cpp").read_text()
        game = (ROOT / "native/src/Game/Game.cpp").read_text()
        called = set(re.findall(r'(?:functionName|gameEndFunction) = "(\w+)"', src))
        called |= set(re.findall(r'(?<![\w])Call\("(\w+)"', src))
        called |= set(re.findall(r'GetPresentation\(\)->Call\("(\w+)"', game))
        called |= set(re.findall(r'const char\* functionName = "(\w+)"', src))
        sync = set(re.findall(r'nlStrCmp<char>\("(\w+)", state->mCurrentFunction\) == 0', src))
        local_ok = {"GameBegin": "walk-out: simulation not started (FixedUpdateTask mSimulationStarted)",
                    "PlayHighlight": "replay: task state is not gameplay",
                    "FadeToStrikerTimes": "after the game end: task state is not gameplay"}
        self.assertEqual(sync, {"GoalCelebration", "MegastrikeEnd", "MegastrikeIntro", "GameEndSuddenDeath", "GameEndNoSuddenDeath", "GameEndMegaStrike"})
        unknown = called - sync - set(local_ok)
        self.assertEqual(unknown, set(), "in-match presentation functions nobody classified: %s" % sorted(unknown))
        self.assertGreaterEqual(len(called & sync), 5)   # the scan found the calls (not vacuous)


if __name__ == "__main__":
    unittest.main()
