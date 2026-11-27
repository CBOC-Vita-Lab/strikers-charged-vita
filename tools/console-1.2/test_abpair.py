#!/usr/bin/env python3
"""Unit tests for abpair.py (standard library only).

Run: python3 tools/console-1.2/test_abpair.py   (or python3 -m unittest)
"""

import contextlib
import io
import json
import os
import random
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import abpair  # noqa: E402


def lane_line(dc, fe, be=(5.0, 9.0), game="-", pics=300):
    return (f"[lane] {pics} pics 12.00s 25.0/s game {game} send 0.4 fe {fe} "
            f"be {be[0]}/{be[1]} gpu 6.0 cores - dc {dc} soft 1 static 0 th 3")


def head_line(cls, pics, step=(20.0, 24.0), render=14.0, gpu=15.0):
    if step is None:
        step = (-1.0, -1.0)
    return (f"[head] 30 s, class {cls} (lock60 1): {pics} pictures (0 with a step, "
            f"{pics} without), late 0 (0.0 %), relief: policy 60 now, 1 pictures at 30, "
            f"8 switches | game ms: with step p50 {step[0]} p95 {step[1]} max 0.0, "
            f"without p50 3.7 p95 4.4 max 15.3, all p95 4.4 (limit 14.5: ok) | "
            f"render thread ms p95 {render} max 0.0 (n 2), GPU ms p95 {gpu} max 0.0 (n 3) | "
            f"over 16.7 ms: game 0 render 0 GPU 0: none")


def gsplit_line(pics, sim, steps, render=6.0):
    return (f"[gsplit] {pics} pics wall 40.0 ms/pic: sim {sim} (steps {steps}/pic: "
            f"pre 0.09 clock 0.00 ai 0.66 npc 0.02 prephys 0.00 physics 1.15 pose 0.01 "
            f"net 0.00 events 0.18 pad 0.12) render-task {render} (pre 0.21 npcs 1.40 "
            f"stage 0.01)")


def gsplit_phys_line(steps, phys):
    return (f"[gsplit-phys] {steps} steps in 300 pics, ms per STEP: physics {phys} "
            f"(self 0.01 precollide 0.07 broad 0.56)")


def gsplit_ai_line(ai):
    return f"[gsplit-ai] ms per STEP: ai {ai} (teampre 0.01 misc 0.07 | fielder: self 0.1)"


def write_log(directory, name, lines):
    path = os.path.join(directory, name)
    with open(path, "w") as fh:
        fh.write("\n".join(lines) + "\n")
    return path


class ParseTests(unittest.TestCase):

    def test_lane_classes_by_draw_count(self):
        self.assertEqual(abpair.parse_lane(lane_line(1, 1.7))["cls"], "light")
        self.assertEqual(abpair.parse_lane(lane_line(179, 1.7))["cls"], "light")
        self.assertEqual(abpair.parse_lane(lane_line(180, 1.7))["cls"], "play")
        self.assertEqual(abpair.parse_lane(lane_line(279, 1.7))["cls"], "play")
        self.assertEqual(abpair.parse_lane(lane_line(280, 1.7))["cls"], "heavy")

    def test_lane_fields_and_missing_game(self):
        w = abpair.parse_lane(lane_line(200, 7.1, be=(4.6, 7.1), game="-"))
        self.assertEqual(w["metrics"]["fe"], 7.1)
        self.assertEqual(w["metrics"]["be_cpu"], 4.6)
        self.assertEqual(w["metrics"]["be_wall"], 7.1)
        self.assertEqual(w["metrics"]["gpu"], 6.0)
        self.assertIsNone(w["metrics"]["game"])
        self.assertEqual(w["pics"], 300)

    def test_lane_game_present(self):
        w = abpair.parse_lane(lane_line(200, 1.0, game="7.9"))
        self.assertEqual(w["metrics"]["game"], 7.9)

    def test_head_filters_small_windows_and_no_data_marker(self):
        self.assertIsNone(abpair.parse_head(head_line("play", 134)))
        w = abpair.parse_head(head_line("cut-scene", 300, step=None))
        self.assertEqual(w["cls"], "cut-scene")
        self.assertIsNone(w["metrics"]["step_p50"])
        self.assertIsNone(w["metrics"]["step_p95"])
        self.assertEqual(w["metrics"]["render_p95"], 14.0)
        self.assertEqual(w["metrics"]["gpu_p95"], 15.0)

    def test_gsplit_per_step_and_live_class(self):
        with tempfile.TemporaryDirectory() as d:
            path = write_log(d, "g.log", [
                gsplit_line(300, 9.74, 1.67),
                gsplit_phys_line(501, 2.76),
                gsplit_ai_line(1.57),
                gsplit_line(300, 0.03, 0.0),
                gsplit_phys_line(0, 0.0),
                gsplit_ai_line(0.0),
            ])
            windows = abpair.parse_file(path)
        live, idle = windows
        self.assertEqual(live["cls"], "live")
        self.assertAlmostEqual(live["metrics"]["sim_step"], 9.74 / 1.67)
        self.assertEqual(live["metrics"]["phys_step"], 2.76)
        self.assertEqual(live["metrics"]["ai_step"], 1.57)
        self.assertEqual(idle["cls"], "non-live")
        self.assertIsNone(idle["metrics"]["sim_step"])
        self.assertIsNone(idle["metrics"]["phys_step"])
        self.assertIsNone(idle["metrics"]["ai_step"])
        self.assertEqual(idle["metrics"]["render_task"], 6.0)


class StatsTests(unittest.TestCase):

    def test_clear_difference_reads_on_better(self):
        rng = random.Random(11)
        off = [rng.gauss(12.0, 1.0) for _ in range(40)]
        on = [v - 2.0 + rng.gauss(0.0, 0.2) for v in off]
        lo, hi = abpair.bootstrap_ci(on, off)
        self.assertLess(lo, hi)
        self.assertLess(hi, 0)
        self.assertEqual(abpair.verdict(on, off, lo, hi), "ON better")

    def test_clear_difference_reads_off_better(self):
        rng = random.Random(12)
        on = [rng.gauss(12.0, 0.5) for _ in range(30)]
        off = [v - 3.0 for v in on]
        lo, hi = abpair.bootstrap_ci(on, off)
        self.assertEqual(abpair.verdict(on, off, lo, hi), "OFF better")

    def test_no_difference_same_distribution(self):
        # Both sides drawn from one distribution: the 90 % interval of the
        # difference must straddle zero.
        pool_rng = random.Random(2024)
        pool = [pool_rng.gauss(10.0, 1.0) for _ in range(60)]
        on = pool[::2]
        off = pool[1::2]
        lo, hi = abpair.bootstrap_ci(on, off)
        self.assertLessEqual(lo, 0.0)
        self.assertGreaterEqual(hi, 0.0)
        self.assertEqual(abpair.verdict(on, off, lo, hi), "no difference")

    def test_no_difference_identical_sides(self):
        values = [9.0, 10.0, 11.0, 12.0, 13.0, 10.5, 9.5, 11.5]
        lo, hi = abpair.bootstrap_ci(values, list(values))
        self.assertEqual(abpair.verdict(values, values, lo, hi), "no difference")

    def test_empty_side_reads_no_data(self):
        self.assertEqual(abpair.verdict([], [1.0, 2.0], 0.0, 0.0), "no data")

    def test_single_window_reads_too_few(self):
        self.assertEqual(abpair.verdict([1.0], [2.0, 3.0], -1.0, -1.0), "too few windows")

    def test_bootstrap_is_deterministic(self):
        values = [1.0, 2.0, 3.0, 4.0]
        self.assertEqual(abpair.bootstrap_ci(values, values[::-1]),
                         abpair.bootstrap_ci(values, values[::-1]))


class EndToEndTests(unittest.TestCase):

    def test_cli_json_no_difference_and_clear_difference(self):
        with tempfile.TemporaryDirectory() as d:
            pool_rng = random.Random(77)
            # Same distribution on both sides: no difference in fe.
            pool = [round(pool_rng.gauss(10.0, 1.0), 3) for _ in range(40)]
            on_fe = pool[::2]
            off_fe = pool[1::2]
            # Clear difference in game time: ON 3 ms lower.
            on_game = [4.0 + 0.01 * i for i in range(20)]
            off_game = [v + 3.0 for v in on_game]
            on_lines = [lane_line(200, on_fe[i], game=f"{on_game[i % 20]:.2f}")
                        for i in range(20)]
            off_lines = [lane_line(200, off_fe[i], game=f"{off_game[i % 20]:.2f}")
                         for i in range(20)]
            on_log = write_log(d, "on2.log", on_lines)
            off_log = write_log(d, "off2.log", off_lines)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = abpair.main(["--on", on_log, "--off", off_log,
                                    "--metric", "fe,game", "--json"])
            self.assertEqual(code, 0)
            doc = json.loads(out.getvalue())
            rows = {(r["metric"], r["class"]): r for r in doc["results"]}
            self.assertEqual(rows[("fe", "play")]["verdict"], "no difference")
            self.assertEqual(rows[("game", "play")]["verdict"], "ON better")
            self.assertEqual(doc["pictures"]["on"]["lane"], 6000)
            self.assertEqual(doc["pictures"]["off"]["lane"], 6000)

    def test_cli_table_and_unknown_metric(self):
        with tempfile.TemporaryDirectory() as d:
            log = write_log(d, "a.log", [lane_line(200, 5.0)])
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(abpair.main(["--on", log, "--off", log]), 0)
            self.assertIn("verdict", out.getvalue())
            self.assertIn("lane fe", out.getvalue())
            with contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    abpair.main(["--on", log, "--off", log, "--metric", "bogus"])


class PairedTests(unittest.TestCase):
    def test_paired_cancels_a_drift_that_the_pooled_test_cannot(self):
        # three run pairs; every run drifts +4 ms from the one before (heat), the arm is worth -1.0 ms. Pooled windows overlap (no difference); paired windows say ON better.
        with tempfile.TemporaryDirectory() as d:
            on, off = [], []
            for i in range(3):
                drift = 4.0 * i
                on.append(write_log(d, f"on{i}.log", [lane_line(200, round(10.0 + drift - 1.0 + 0.1 * k, 2)) for k in range(4)]))
                off.append(write_log(d, f"off{i}.log", [lane_line(200, round(10.0 + drift + 0.1 * k, 2)) for k in range(4)]))
            rows_p, _ = abpair.analyse(on, off, ["fe"])
            rows_m, _ = abpair.analyse_paired(on, off, ["fe"])
            self.assertEqual(rows_p[0]["verdict"], "no difference")
            self.assertEqual(rows_m[0]["verdict"], "ON better")
            self.assertAlmostEqual(rows_m[0]["diff"], -1.0, places=6)
            self.assertEqual(rows_m[0]["n_on"], 12)

    def test_paired_mutant_swapped_arms_flips_the_verdict(self):
        with tempfile.TemporaryDirectory() as d:
            a = [write_log(d, f"a{i}.log", [lane_line(200, 9.0 + 4 * i) for _ in range(3)]) for i in range(2)]
            b = [write_log(d, f"b{i}.log", [lane_line(200, 10.0 + 4 * i) for _ in range(3)]) for i in range(2)]
            self.assertEqual(abpair.analyse_paired(a, b, ["fe"])[0][0]["verdict"], "ON better")
            self.assertEqual(abpair.analyse_paired(b, a, ["fe"])[0][0]["verdict"], "OFF better")

    def test_paired_one_window_is_too_few(self):
        with tempfile.TemporaryDirectory() as d:
            a = [write_log(d, "a.log", [lane_line(200, 9.0)])]
            b = [write_log(d, "b.log", [lane_line(200, 10.0)])]
            self.assertEqual(abpair.analyse_paired(a, b, ["fe"])[0][0]["verdict"], "too few windows")


def present_line(pics, score, secs=10.0):
    return (f"[present] aligned, {secs} s, {pics} pictures ({pics} stamped); latency 4 vblanks (+0 -0); ready after 0/1/2/3/4/5/6/7+ vblanks 0/0/0/0/600/0/0/0; "
            f"late 0 (0 beyond the largest); held 0/1/2/3/4+ vblanks 0/0/600/0/0; game steps 1/2/3/4+ vblanks 0/600/0/0; judder 0.00 vblanks a picture; "
            f"shown-minus-start 4.00 +- 0.00 vblanks; waits 0; hold mix (percent of pictures shown 1 / 2 / 3+ refreshes) live play none; all scenes 0.0 / 100.0 / 0.0 % of 600; "
            f"smooth {score}/100 (ideal 100 %, judder 0.00, missed 0.00/s)")


class PresentScoreTests(unittest.TestCase):
    def test_parse_present(self):
        w = abpair.parse_present(present_line(600, 87))
        self.assertEqual((w["source"], w["cls"], w["pics"], w["metrics"]["smooth"]), ("present", "all", 600, 87.0))
        self.assertIsNone(abpair.parse_present(present_line(50, 87)))          # too few pictures
        self.assertIsNone(abpair.parse_present(present_line(600, 87).split("; smooth")[0]))   # an old log without the token

    def test_higher_is_better_verdict(self):
        with tempfile.TemporaryDirectory() as d:
            on = write_log(d, "on.log", [present_line(600, 90 + i % 3) for i in range(10)])
            off = write_log(d, "off.log", [present_line(600, 70 + i % 3) for i in range(10)])
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(abpair.main(["--on", on, "--off", off, "--metric", "smooth", "--json"]), 0)
            rows = json.loads(out.getvalue())["results"]
            self.assertEqual(rows[0]["verdict"], "ON better")       # +20 points is better, not worse
            self.assertGreater(rows[0]["diff"], 15)
        self.assertEqual(abpair.verdict([1, 2], [1, 2], 1.0, 3.0, True), "ON better")
        self.assertEqual(abpair.verdict([1, 2], [1, 2], 1.0, 3.0, False), "OFF better")


def ab_lane(pics, secs, fe, tag, gpu=10.0, be=(5.0, 9.0), dc=200):
    return (f"[lane] {pics} pics {secs:.2f}s {pics / secs:.1f}/s game - send 0.4 fe {fe} be {be[0]}/{be[1]} gpu {gpu} cores - dc {dc} soft 1 static 0 th 3{tag}")


class InRunTests(unittest.TestCase):
    """wave 15 (MSC_AB): the b windows against the a windows beside them, inside one run."""

    def run_log(self, d, name, fe_a, fe_b, drift=0.0, key="MSC_VGX_PX_NOOP", windows=8, fps_a=30.0, fps_b=30.0):
        lines = ["[ab] MSC_AB: start", ab_lane(300, 10.0, 9.0, " ab-pre %s=1 a w0" % key)]
        for w in range(windows):
            arm = w % 2
            fe = (fe_b if arm else fe_a) + drift * w
            fps = fps_b if arm else fps_a
            lines.append(ab_lane(int(fps * 1.0), 1.0, fe, " ab-settle %s=%d %s w%d" % (key, 0 if arm else 1, "b" if arm else "a", w)))
            lines.append(ab_lane(int(fps * 5.0), 5.0, fe, " ab %s=%d %s w%d" % (key, 0 if arm else 1, "b" if arm else "a", w)))
        return write_log(d, name, lines)

    def rows(self, files, extra=()):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(abpair.main(["--inrun", *files, "--json", *extra]), 0)
        return {(r["key"], r["metric"]): r for r in json.loads(out.getvalue())["results"]}

    def test_parse_skips_settle_and_pre(self):
        with tempfile.TemporaryDirectory() as d:
            f = self.run_log(d, "r.log", 9.0, 11.0)
            wins = abpair.parse_inrun(f)
            self.assertEqual(sorted(w for (_, w) in wins), list(range(8)))        # 8 measured windows; the settle lines (same numbers) and the pre line are not in them
            self.assertEqual([wins[("MSC_VGX_PX_NOOP", w)]["arm"] for w in range(4)], [0, 1, 0, 1])
            self.assertEqual(wins[("MSC_VGX_PX_NOOP", 0)]["pics"], 150)

    def test_split_window_is_put_together(self):
        with tempfile.TemporaryDirectory() as d:
            f = write_log(d, "s.log", [ab_lane(100, 3.0, 8.0, " ab K=1 a w0"), ab_lane(50, 2.0, 11.0, " ab K=1 a w0")])
            w = abpair.parse_inrun(f)[("K", 0)]
            self.assertEqual((w["pics"], round(w["secs"], 2)), (150, 5.0))
            self.assertAlmostEqual(w["v"]["fe"], (8.0 * 100 + 11.0 * 50) / 150)

    def test_b_slower_by_two_ms_is_found(self):
        with tempfile.TemporaryDirectory() as d:
            r = self.rows([self.run_log(d, "r.log", 9.0, 11.0)])[("MSC_VGX_PX_NOOP", "fe")]
            self.assertAlmostEqual(r["diff"], 2.0, places=6)
            self.assertEqual((r["n"], r["verdict"]), (4, "OFF better"))      # b (ON) is 2 ms slower in every window: the interval sits above zero, lower is better
    def test_drift_cancels(self):
        # the machine heats up: +0.5 ms of fe every window. b is truly +1.0 ms. A run of 9 windows (a b a b a b a b a) gives every b two a neighbours: b(w) - mean(a(w-1), a(w+1)) = 1.0 exactly
        # (the drift is linear). 8 windows (the last b has one neighbour) is off by the drift there only: 1.125, still nearer than the plain mean of all b minus all a (1.5).
        with tempfile.TemporaryDirectory() as d:
            r = self.rows([self.run_log(d, "r.log", 9.0, 10.0, drift=0.5, windows=9)])[("MSC_VGX_PX_NOOP", "fe")]
            self.assertAlmostEqual(r["diff"], 1.0, places=6)
            r8 = self.rows([self.run_log(d, "r8.log", 9.0, 10.0, drift=0.5, windows=8)])[("MSC_VGX_PX_NOOP", "fe")]
            wins = abpair.parse_inrun(os.path.join(d, "r8.log"))
            a = [w["v"]["fe"] for (k, n), w in wins.items() if w["arm"] == 0]
            b = [w["v"]["fe"] for (k, n), w in wins.items() if w["arm"] == 1]
            naive = sum(b) / len(b) - sum(a) / len(a)
            self.assertLess(abs(r8["diff"] - 1.0), abs(naive - 1.0))

    def test_no_effect_reads_no_difference_and_fps_is_higher_better(self):
        with tempfile.TemporaryDirectory() as d:
            same = self.rows([self.run_log(d, "r.log", 9.0, 9.0, drift=0.1, windows=11)])[("MSC_VGX_PX_NOOP", "fe")]
            self.assertAlmostEqual(same["diff"], 0.0, places=6)
            f = self.run_log(d, "g.log", 9.0, 9.0, windows=11, fps_a=30.0, fps_b=33.0)
            r = self.rows([f])[("MSC_VGX_PX_NOOP", "fps")]
            self.assertAlmostEqual(r["diff"], 3.0, places=6)
            self.assertIn(r["verdict"], ("ON better", "no difference"))
            self.assertEqual(abpair.verdict([33, 33], [30, 30], 2.0, 4.0, True), "ON better")

    def test_too_few_and_no_data(self):
        with tempfile.TemporaryDirectory() as d:
            r = self.rows([self.run_log(d, "r.log", 9.0, 11.0, windows=2)])[("MSC_VGX_PX_NOOP", "fe")]
            self.assertEqual((r["n"], r["verdict"]), (1, "too few windows"))
            plain = write_log(d, "p.log", [lane_line(200, 9.0)])
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(abpair.main(["--inrun", plain]), 0)
            self.assertIn("no matched windows", out.getvalue())

    def test_scene_change_neighbour_is_left_out(self):
        # windows 0..4 = a b a b a at dc 200; the middle a (2) is a replay (dc 500, fe 30): without the guard it would drag the difference; with it b(1) and b(3) use their other neighbour
        with tempfile.TemporaryDirectory() as d:
            lines = []
            for w in range(5):
                arm = w % 2
                dc, fe = (500, 30.0) if w == 2 else (200, 11.0 if arm else 9.0)
                lines.append(ab_lane(150, 5.0, fe, " ab K=%d %s w%d" % (0 if arm else 1, "b" if arm else "a", w), dc=dc))
            f = write_log(d, "sc.log", lines)
            r, pics = abpair.analyse_inrun([f], ["fe"])
            self.assertAlmostEqual(r[0]["diff"], 2.0, places=6)
            self.assertEqual((pics["dropped"]["scene"], pics["dropped"]["used"]), (2, 2))
            r0, _ = abpair.analyse_inrun([f], ["fe"], dc_tol=0)
            self.assertLess(r0[0]["diff"], -5.0)                              # the guard off: the replay window is in the baseline and b reads 8.5 ms faster

    def test_cli_rules(self):
        with tempfile.TemporaryDirectory() as d:
            f = self.run_log(d, "r.log", 9.0, 11.0)
            with contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    abpair.main(["--inrun", f, "--on", f])
                with self.assertRaises(SystemExit):
                    abpair.main(["--inrun", f, "--metric", "smooth"])
                with self.assertRaises(SystemExit):
                    abpair.main([])


if __name__ == "__main__":
    unittest.main()
