#!/usr/bin/env python3
"""Tests of the console kit: generated files are current, every env line is one the isolated build's reader accepts and one the source
reads, the analysis gives the right table on synthetic logs, the session driver does what it says against a fake console (no network).

    python3 -m unittest tools/console-1.2/test_kit.py        (or: python3 tools/console-1.2/test_kit.py)
"""
import csv
import importlib.util
import io
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))

import analyse as A  # noqa: E402
import matrix as M  # noqa: E402
import synth as S  # noqa: E402
import night as N  # noqa: E402
import gen_routes as R  # noqa: E402


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def run(*cmd, **kw):
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)


def env_lines(path):
    for line in Path(path).read_text().splitlines():
        if line.strip() and not line.startswith("#"):
            yield line


class Generated(unittest.TestCase):
    def test_files_are_current(self):
        for script in ("gen_kit.py", "gen_routes.py"):
            r = run(sys.executable, HERE / script, "--check")
            self.assertEqual(r.returncode, 0, script + ": " + r.stdout + r.stderr)

    def test_budget_and_tiers(self):
        total, rows = M.estimate(1, True)
        self.assertLessEqual(total, M.BUDGET_MIN)
        ids = [r["id"] for r in M.RUNS]
        self.assertEqual(len(ids), len(set(ids)))
        for r in M.RUNS:
            self.assertTrue((HERE / "env" / (r["id"] + ".txt")).is_file())
        refs = [r["id"] for r in M.RUNS if r["role"] == "ref"]
        self.assertEqual(refs, ["d1", "megad1", "rep1", "rep2", "pipe1", "pipe2", "dmid", "d2", "megad2"])
        order = [i["id"] for k, i in M.session_order(1) if k == "run"]
        self.assertLess(order.index("d1"), order.index("msaa0"))
        self.assertGreater(order.index("d2"), order.index("r10m"))     # the second reference closes the 1.2 runs after the older versions
        for needed in ("msaa0", "fg0", "fxn", "aniso0", "timing0", "core0off", "exact0", "r12", "r11", "r10"):
            self.assertIn(needed, order)
        # the questions of 1.3, in the order of expected value, before the older switches
        wanted = ["softx128", "softx64", "gxmdirect1", "between", "snaponce0", "posecull0", "crowdposeonce0", "crowdrefresh1", "npclazy0", "promosolo8", "dltiny64", "fxglowhalf",
                  "shadow2", "farlod12", "fxruns2", "clockmax", "pipecensus", "pipe1"]
        at = [order.index(i) for i in wanted]
        self.assertEqual(at, sorted(at))
        self.assertLess(at[-1], order.index("msaa0"))

    def test_routes_of_13(self):
        by_id = {r["id"]: r for r in M.RUNS}
        def env(i):
            return dict(by_id[i]["env"])
        self.assertEqual(env("softx128"), {"MSC_VGX_SOFTXFORM": "128"})
        self.assertEqual(env("softx64"), {"MSC_VGX_SOFTXFORM": "64"})
        self.assertEqual(env("gxmdirect1"), {"MSC_VGX_GXM_DIRECT": "1"})
        self.assertEqual(env("between"), {"MSC_FPS_BETWEEN": "pace"})
        self.assertEqual(env("clockmax"), {"MSC_VITA_CLOCK_POLICY": "max"})
        # a stadium's own reference pair and census: replays against replays, Pipeline Central against itself, the census out of the table
        for rid in ("shadow2", "farlod12", "rep1", "rep2"):
            self.assertEqual(by_id[rid]["route"], "replay")
        for rid in ("pipe1", "pipe2", "pipecensus"):
            self.assertEqual(by_id[rid]["route"], "pipe")
        self.assertEqual((by_id["pipecensus"]["role"], by_id["ctrlcensus"]["route"]), ("census", "ctrl"))
        self.assertEqual(env("pipecensus")["MSC_STADIUM_CENSUS"], "1")
        self.assertEqual(env("pipecensus"), env("ctrlcensus"))
        # the census's picture dumps fall inside the route's open play (frames: the route's walk-out and play at 30 or 60 fps)
        self.assertTrue(all(1400 < int(n) < 2600 for n in env("pipecensus")["MSC_VGX_PASSES"].split(",")))
        # the stadium ids are the game's, the routes use only commands the tour and the look route already use
        enum = (REPO / "native/include/Game/DB/BasicGameInfo.h").read_text()
        self.assertIn("STAD_PIPELINE_CENTRAL = 8,", enum)
        self.assertIn("STAD_BOWSER_STADIUM = 1,", enum)
        def words(name):
            return {l.split()[0] for l in (HERE / "routes" / name).read_text().splitlines() if l.strip() and not l.startswith("#")}
        known = words("tour.txt") | words("look.txt")
        for name, stadium in (("pipe.txt", 8), ("ctrl.txt", 1), ("replay.txt", 13)):
            self.assertLessEqual(words(name), known, name)
            self.assertIn("stadium=%d " % stadium, (HERE / "routes" / name).read_text())
        # if and endif balance, and do not nest (the routes rely on neither)
        for name in ("pipe.txt", "replay.txt"):
            depth = 0
            for l in (HERE / "routes" / name).read_text().splitlines():
                depth += l.startswith("if ") - l.startswith("endif")
                self.assertIn(depth, (0, 1))
            self.assertEqual(depth, 0)
        # every route file is staged
        script = (HERE / "session.sh").read_text()
        for route in {r["route"] for r in M.RUNS} - {"settings", "gsvice", "gsdump", "gslava", "megafull", "sand", "pause", "live19", "live19c", "live19r", "live19l", "onhub", "onconn", "onquick", "onranked", "onlists", "onhostwait", "lanwifi", "lanadhoc"}:     # (the Settings run never reaches its script; the goal routes are fast-boot only, uploaded from fastboot/routes/)
            self.assertRegex(script, r'ROUTES="[^"]*\b%s\b' % route)
        # (1.3 prune: the Settings screen no longer writes PERFORMANCE; the kit's pairs stand on their own, every reader stays in the game)

    def test_switches_asked_for(self):
        def env(i):
            return dict(M.RUNS[[r["id"] for r in M.RUNS].index(i)]["env"])
        # the 1.2 defaults have every item on: the A/B runs write the value that turns each one off
        self.assertEqual(env("msaa0"), {"MSC_VGX_MSAA": "0"})
        self.assertEqual(env("fg0"), {"MSC_DYNRES_FG": "0"})
        self.assertEqual(env("aniso0"), {"MSC_VITA_ANISO": "0"})
        self.assertIn("MSC_FX_LOWRES_SCENES", env("fxn"))
        self.assertEqual(env("timing0"), {"MSC_VITA_QUEUE_FIXED30": "0", "MSC_VITA_PRIO_VBLANK": "0", "MSC_VITA_CORE0_ALONE": "0", "MSC_PACE_VBLANK": "0", "MSC_SLIP": "0"})
        self.assertEqual(env("exact0"), {"MSC_VGX_SHQUAD": "0", "MSC_VGX_SOFTXFORM": "128", "MSC_DOF": "1"})   # (SOFTXFORM=0 would be no CPU transform at all; DOF=0 no blur)
        self.assertEqual(env("slip0"), {"MSC_SLIP": "0"})
        self.assertEqual(env("core0off"), {"MSC_VITA_CORE0_ALONE": "0"})
        # the five pacing items off together: every default the player build gives to a pacing switch (vita_plan.cpp kQuality)
        self.assertEqual(set(env("timing0")), {"MSC_VITA_QUEUE_FIXED30", "MSC_VITA_PRIO_VBLANK", "MSC_VITA_CORE0_ALONE", "MSC_PACE_VBLANK", "MSC_SLIP"})
        self.assertEqual(env("dof0"), {"MSC_DOF": "0"})
        self.assertEqual(env("softx128"), {"MSC_VGX_SOFTXFORM": "128"})
        lan = {leg["id"]: dict(leg["host_env"]) for leg in M.LAN_LEGS}
        self.assertEqual(lan["lan-auto"], {"MSC_LAN_INPUT_DELAY": "auto"})
        # the same route and logging switches in every tour run, no clock or frame-rate switch
        for r in M.RUNS:
            text = (HERE / "env" / (r["id"] + ".txt")).read_text()
            self.assertIn("MSC_FRAME_TIMING=300", text)
            self.assertEqual(len(re.findall(r"^MSC_INPUT_SCRIPT=", text, re.M)), 1)
            # clock17: every run is the player's clock policy (max: a deferred 500 request, 444 if refused) once; the stock-clock A/B runs add MSC_VITA_CPU_MHZ=444 on top of it
            self.assertEqual(len(re.findall(r"(?m)^MSC_VITA_CLOCK_POLICY=", text)), 1, r["id"])
            self.assertIn("MSC_VITA_CLOCK_POLICY=max", text, r["id"])
            if r["id"] in ("clock444", "clock444d"):
                self.assertIn("MSC_VITA_CPU_MHZ=444", text, r["id"])
                self.assertEqual(r["tier"], 2, r["id"])
                self.assertEqual(r["group"], "clock", r["id"])
            if r["id"] not in ("gpu166", "bus166", "xbar111", "clockdefer0", "clock444", "clock444d"):     # (the three that lower one clock to see what limits the GPU, and the one about when the policy asks)
                self.assertNotRegex(text, r"(?m)^MSC_VITA_(CPU|GPU|BUS|XBAR|STOCK_SIM|CLOCK_(DEFER|KEEP|WATCH|MWATCH))", r["id"])
            if r["id"] in ("clock444", "clock444d"):     # clockfix17: the 444 is asked 1 s into the process too, after the PSVshellPlus profile
                self.assertEqual(re.findall(r"(?m)^MSC_VITA_CLOCK_DEFER=(.*)$", text), ["1"], r["id"])
            # the fast-boot env files (what the sitting uses with `fastboot 1`) carry the same one policy line
            fb = HERE / "env-fb" / (r["id"] + ".txt")
            if fb.exists():
                self.assertEqual(re.findall(r"(?m)^MSC_VITA_CLOCK_POLICY=(.*)$", fb.read_text()), ["max"], r["id"] + " (env-fb)")
            # Galaxy's lesson (13 Nov): settings and env files take ONE NAME=value per line; two on a line wasted a run there
            for line in text.splitlines():
                if line.strip() and not line.lstrip().startswith("#"):
                    self.assertRegex(line, r"^[A-Z][A-Z0-9_]*=", r["id"] + ": " + line)
                    self.assertEqual(len(re.findall(r"(?:^|\s)[A-Z][A-Z0-9_]{2,}=", line)), 1, r["id"] + ": more than one assignment on a line: " + line)

    def test_runs_turn_the_build_defaults_off(self):
        """The 1.2 defaults (vita_plan.cpp: quality and timing switches) are on in the build, so a 1.2 run that names one of those keys must write a value
        that differs from the default, and every default is switched off by at least one run (the A/B of the owner's 'best experience by default')."""
        plan = (REPO / "native/platform/vita/src/vita_plan.cpp").read_text()
        block = plan[plan.index("kQuality[][2]"):]
        block = block[:block.index("};")]
        defaults = dict(re.findall(r'\{"([A-Z0-9_]+)", "([^"]*)"\}', block))
        defaults.update(re.findall(r'add\("(MSC_VGX_MSAA)", "([^"]*)"\)', plan))
        self.assertGreaterEqual(len(defaults), 11)
        turned_off = set()
        for r in M.RUNS:
            if r["version"] != "1.3":
                continue
            for key, value in r["env"]:
                if key in defaults:
                    self.assertNotEqual(value, defaults[key], "%s writes the default %s=%s" % (r["id"], key, value))
                    turned_off.add(key)
        self.assertEqual(sorted(set(defaults) - turned_off), [], "defaults no run turns off")

    def test_no_machine_paths_in_the_kit(self):
        bad = re.compile(r"/home/|/Users/|/root/|/mnt/|C:\\\\|172\.(16|20)\.|192\.168\.")
        for path in HERE.rglob("*"):
            if path.is_file() and path.suffix in {".py", ".sh", ".txt", ".json", ".md"} and "__pycache__" not in path.parts:
                for n, line in enumerate(path.read_text().splitlines(), 1):
                    if path.name == "test_kit.py" and "bad = re.compile" in line:
                        continue
                    self.assertIsNone(bad.search(line), "%s:%d: %s" % (path.relative_to(HERE), n, line[:100]))


class Accepted(unittest.TestCase):
    """Every NAME=VALUE the kit writes passes the isolated build's reader (NpHardwareAccept) and is read by the source."""

    @classmethod
    def setUpClass(cls):
        cls.tool = load(REPO / "tools/stage-hardware-test.py", "stage_tool")
        cls.tmp = Path(tempfile.mkdtemp())
        (cls.tmp / "np_hardware_isolation.h").write_text(cls.tool.guard_header(cls.tool.DISC))
        (cls.tmp / "t.cpp").write_text('#include "np_hardware_isolation.h"\n#include <iostream>\n#include <string>\n'
                                       'int main(){std::string l;int bad=0;while(std::getline(std::cin,l)){auto p=l.find(\'=\');if(p==std::string::npos)continue;'
                                       'if(!NpHardwareAccept(l.substr(0,p).c_str(),l.substr(p+1).c_str())){std::cout<<"REJECT "<<l<<"\\n";++bad;}}return bad?1:0;}\n')
        cxx = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
        cls.exe = None
        if cxx:
            r = run(cxx, "-std=c++11", "-I", cls.tmp, cls.tmp / "t.cpp", "-o", cls.tmp / "t")
            if r.returncode == 0:
                cls.exe = cls.tmp / "t"

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def files(self):
        return sorted(list((HERE / "env").glob("*.txt")) + list((HERE / "lan").glob("*.txt")))

    def test_reader_accepts_every_line(self):
        if not self.exe:
            self.skipTest("no C++ compiler")
        for path in self.files():
            r = subprocess.run([str(self.exe)], input="\n".join(env_lines(path)) + "\n", capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, "%s: %s" % (path.name, r.stdout))

    def test_script_must_be_under_the_private_root(self):
        if not self.exe:
            self.skipTest("no C++ compiler")
        bad = subprocess.run([str(self.exe)], input="MSC_INPUT_SCRIPT=ux0:data/mscharged/tour.txt\n", capture_output=True, text=True)
        self.assertEqual(bad.returncode, 1)

    def test_every_key_is_read_by_the_source(self):
        keys = set()
        for path in self.files():
            for line in env_lines(path):
                keys.add(line.split("=", 1)[0])
        sources = []
        for root in ("native", "renderer", "dependencies/vitaGL/source", "src"):
            base = REPO / root
            if base.is_dir():
                for p in base.rglob("*"):
                    if p.suffix in {".cpp", ".c", ".h", ".hpp"}:
                        sources.append(p)
        blob = "\n".join(p.read_text(errors="replace") for p in sources)
        self.assertGreater(len(blob), 1000)
        import night as N
        for key in sorted(keys):
            if key in N.PENDING:  # a switch of another agent's branch: the night drops its runs until the source reads it
                continue
            self.assertIn('"%s"' % key, blob, key + " is not read by any source file")

    def test_values_have_the_form_the_reader_wants(self):
        # numeric / enumerated values checked against what the readers take (docs/TECHNICAL.md, np_settings.cpp)
        allowed = {"MSC_VGX_MSAA": {"0", "1", "2", "4", "8"}, "MSC_DOF": {"0", "1"}, "MSC_DYNRES_FG": {"0", "1"}, "MSC_VITA_ANISO": {"0", "1", "2", "3"},
                   "MSC_FX_LOWRES_SCENES": {"nis", "all", "none", "play", "nis,play"}, "MSC_LAN_INPUT_DELAY": {"auto"} | {str(i) for i in range(1, 21)},
                   "MSC_LAN_MODE": {"wifi", "adhoc"}}
        for path in self.files():
            for line in env_lines(path):
                k, v = line.split("=", 1)
                if k in allowed:
                    self.assertIn(v, allowed[k], "%s: %s" % (path.name, line))


# ---- analysis ---------------------------------------------------------------------------------------------------------------------------

class Analysis(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        S.session(cls.tmp)
        cls.plan, cls.runs, cls.stats, cls.dropped, cls.table, cls.refs = A.analyse(cls.tmp)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def row(self, scene, rid):
        return self.table[scene]["rows"][rid]

    def test_noise_is_twice_the_reference_spread(self):
        sec = self.table["play"]
        a, b = (self.stats[r]["play"]["p50"] for r in ("d1", "d2"))
        self.assertAlmostEqual(sec["noise"]["p50"], max(2 * abs(a - b), A.DEFAULT_FLOORS["p50"]), places=6)
        self.assertEqual(sec["refs"], ["d1", "d2"])
        mega = self.table["mega-save"]
        self.assertEqual(mega["refs"], ["megad1", "megad2"])

    def test_known_effects_are_found_and_nothing_else(self):
        self.assertEqual(self.row("play", "msaa0")["p50_v"], "!")
        self.assertAlmostEqual(self.row("play", "msaa0")["p50_d"], 2.0, delta=0.35)
        self.assertEqual(self.row("goal:nis", "fxn")["p50_v"], "*")
        self.assertEqual(self.row("goal:replay", "fxn")["p50_v"], "*")
        self.assertEqual(self.row("play", "fxn")["p50_v"], "")
        self.assertEqual(self.row("play", "r11")["p50_v"], "!")
        self.assertEqual(self.row("mega-save", "r10m")["p50_v"], "!")
        self.assertEqual(self.row("play", "timing0")["p50_v"], "")

    def test_references_carry_no_verdict(self):
        for scene, sec in self.table.items():
            for rid in sec["refs"]:
                self.assertEqual(sec["rows"][rid]["p50_v"], "")

    def test_compile_windows_are_left_out(self):
        self.assertEqual(self.row("play", "d1")["windows"], 6)
        self.assertEqual(self.row("play", "timing0")["windows"], 5)       # the compile at present 3700 sits in play
        self.assertEqual(self.dropped["timing0"]["play"]["compile"], 1)
        self.assertEqual(sum(sum(v.values()) for v in self.dropped["d1"].values()), 0)

    def test_sixty_fps_windows_are_merged_to_ten_seconds(self):
        row = self.row("menu-main", "r11")
        self.assertGreater(row["fps"], 55)
        self.assertGreaterEqual(row["windows"], 1)
        self.assertGreaterEqual(row["seconds"], 9.5)

    def test_short_scenes_are_pooled(self):
        self.assertEqual(self.row("goal:nis", "d1")["type"], "S")
        self.assertEqual(self.row("play", "d1")["type"], "W")

    def test_a_scene_only_where_the_route_has_it(self):
        self.assertNotIn("mega-save", [s for s, sec in self.table.items() if "d1" in sec["rows"]])
        self.assertNotIn("play", [s for s, sec in self.table.items() if "megad1" in sec["rows"]])

    def test_table_prints_once_with_every_run(self):
        out = io.StringIO()
        A.print_table(self.plan, self.runs, self.stats, self.dropped, self.table, self.refs, out=out)
        text = out.getvalue()
        for rid in ("d1", "d2", "msaa0", "fxn", "timing0", "r11", "megad1", "r10m"):
            self.assertIn(rid, text)
        self.assertIn("== runs", text)
        self.assertIn("noise", text)

    def test_empty_and_quiet_logs_do_not_break_the_table(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            shutil.copytree(self.tmp / "d1", d / "d1")
            shutil.copytree(self.tmp / "d2", d / "d2")
            (d / "msaa0").mkdir()
            (d / "msaa0" / "log.txt").write_text("")
            (d / "fxn").mkdir()
            (d / "fxn" / "log.txt").write_text("[vita] ===== Mario Strikers Charged, PS Vita native port, build quiet =====\n[run] first frame: build quiet; clocks ARM 444 MHz\n")
            plan, runs, stats, dropped, table, refs = A.analyse(d)
            out = io.StringIO()
            A.print_table(plan, runs, stats, dropped, table, refs, out=out)
            self.assertIn("fxn", out.getvalue())
            self.assertEqual(stats["msaa0"], {})

    def test_the_last_header_starts_the_run(self):
        old = S.gen_run("x", "tour", seed=5, ms_by_name={"play": 60.0})
        new = S.gen_run("x", "tour", seed=6)
        run_ = A.Run.parse("x", old + new)
        w, p, d = A.build_windows(run_, 10.0, False)
        self.assertAlmostEqual(A.summarise(w, p)["play"]["p50"], 33.4, delta=0.4)

    def test_cutscene_inside_the_play_mark_is_a_goal_sequence(self):
        saved = S.TOUR
        try:
            S.TOUR = [("play", 1200, 9, 5, 2, 33.4, "play"), ("goal", 600, 9, 2, 16, 40.0, "play"), ("play2", 1200, 9, 5, 2, 33.4, "play")]
            text = S.gen_run("x", "tour", seed=2)
        finally:
            S.TOUR = saved
        run_ = A.Run.parse("x", text)
        w, p, d = A.build_windows(run_, 10.0, False)
        s = A.summarise(w, p)
        self.assertIn("goal:nis", s)
        self.assertAlmostEqual(s["goal:nis"]["p50"], 40.0, delta=0.6)
        self.assertAlmostEqual(s["play"]["p50"], 33.4, delta=0.6)

    def test_clock_mismatch_is_flagged(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            for rid, clk in (("d1", "ARM 444 MHz, GPU 222 MHz, bus 222 MHz, GPU xbar 166 MHz"), ("d2", "ARM 333 MHz, GPU 222 MHz, bus 222 MHz, GPU xbar 166 MHz")):
                (d / rid).mkdir()
                (d / rid / "log.txt").write_text(S.gen_run(rid, "tour", seed=3, clocks=clk))
            plan, runs, stats, dropped, table, refs = A.analyse(d)
            out = io.StringIO()
            A.print_table(plan, runs, stats, dropped, table, refs, out=out)
            self.assertRegex(out.getvalue(), r"(?i)clock")

    def test_a_routes_scenes_stay_on_their_own_rows(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            logs = {"pipe1": dict(route="pipe", seed=41), "pipe2": dict(route="pipe", seed=42), "pipecensus": dict(route="pipe", seed=43, ms_by_name={"play": 400.0}),
                    "rep1": dict(route="replay", seed=44), "rep2": dict(route="replay", seed=45), "shadow2": dict(route="replay", seed=46, ms_by_name={"replay": 28.0}),
                    "d1": dict(route="tour", seed=47), "d2": dict(route="tour", seed=48)}
            for rid, kw in logs.items():
                (d / rid).mkdir()
                (d / rid / "log.txt").write_text(S.gen_run(rid, **kw))
            plan, runs, stats, dropped, table, refs = A.analyse(d)
            self.assertEqual(table["pipe-play"]["refs"], ["pipe1", "pipe2"])
            self.assertNotIn("pipe1", table["play"]["rows"])
            self.assertAlmostEqual(table["pipe-play"]["rows"]["pipe1"]["p50"], 150.0, delta=8)
            self.assertEqual(table["rep:replay"]["refs"], ["rep1", "rep2"])
            self.assertEqual(table["rep:replay"]["rows"]["shadow2"]["p50_v"], "*")
            self.assertNotIn("goal:replay", stats["rep1"])
            self.assertEqual(stats["pipecensus"], {})       # void, and not a row anywhere
            out = io.StringIO()
            A.print_table(plan, runs, stats, dropped, table, refs, out=out)
            self.assertIn("census: frame times void", out.getvalue())
            self.assertNotIn("pipecensus lacks", out.getvalue())

    def test_csv(self):
        path = self.tmp / "t.csv"
        A.write_csv(str(path), self.table)
        with open(path) as f:
            rows = list(csv.DictReader(f))
        self.assertTrue(any(r["scene"] == "play" and r["run"] == "msaa0" and "p50!" in r["verdicts"] for r in rows))

    def test_lan_report(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            host = S.gen_run("h", "tour", seed=7) + "[mp-lan] input delay: auto from a 24 ms round trip (tick 16.7 ms, margin 4 ms): 3 tick(s)\n"
            join = S.gen_run("j", "tour", seed=8) + "[mp-lan] connection lost (peer)\n"
            (d / "h.txt").write_text(host)
            (d / "j.txt").write_text(join)
            out = io.StringIO()
            A.lan_report(str(d / "h.txt"), str(d / "j.txt"), out=out)
            text = out.getvalue()
            self.assertIn("input delay: auto from a 24 ms", text)
            self.assertIn("PROBLEM", text)
            self.assertIn("play:", text)


# ---- session driver against a fake console ----------------------------------------------------------------------------------------------------

CURL = r'''#!/usr/bin/env python3
import os, shutil, sys
root = os.environ["FAKE_ROOT"]
a = sys.argv[1:]
out = up = q = url = None
lst = False
i = 0
while i < len(a):
    x = a[i]
    if x in ("--max-time", "-m"): i += 1
    elif x == "-o": out = a[i + 1]; i += 1
    elif x == "-T": up = a[i + 1]; i += 1
    elif x == "-Q": q = a[i + 1]; i += 1
    elif x == "-l": lst = True
    elif x.startswith("ftp://"): url = x
    i += 1
def fs(p): return os.path.join(root, "fs", p.replace(":", "").strip("/"))
log = open(os.path.join(root, "curl.log"), "a")
path = url.split("/", 3)[3] if url.count("/") >= 3 else ""
if q:
    p = q.split(" ", 1)[1]
    log.write("DELE %s\n" % p)
    if os.path.exists(fs(p)): os.remove(fs(p))
    sys.exit(0)
if up:
    log.write("PUT %s\n" % path)
    if os.environ.get("FAKE_FAIL_PUT") and os.environ["FAKE_FAIL_PUT"] in path: sys.exit(55)
    os.makedirs(os.path.dirname(fs(path)), exist_ok=True)
    shutil.copy(up, fs(path))
    sys.exit(0)
if lst:
    log.write("LIST %s\n" % path)
    d = fs(path)
    if os.path.isdir(d): print("\n".join(sorted(os.listdir(d))))
    sys.exit(0)
log.write("GET %s\n" % path)
if not os.path.isfile(fs(path)): sys.exit(78)
if out: shutil.copy(fs(path), out)
else: sys.stdout.write(open(fs(path)).read())
'''

NC = r'''#!/usr/bin/env python3
import os, sys
root = os.environ["FAKE_ROOT"]
line = sys.stdin.readline().strip()
open(os.path.join(root, "nc.log"), "a").write(line + "\n")
priv = os.path.join(root, "fs", "ux0", "data", "mscharged-perf-test")
if line == "version":
    print("vitacompanion fake 0.1")
elif line == "launch CDXMP0001":
    first = open(os.path.join(priv, "np-player.txt")).readline()
    key = first.split("run ")[1].split(":")[0] if " run " in first else first.split()[2].rstrip(",")
    src = os.path.join(os.environ["FAKE_LOGS"], key + ".txt")
    with open(os.path.join(priv, "log.txt"), "a") as f:
        f.write(open(src).read())
    if key == "cur-setes":
        os.makedirs(os.path.join(priv, "shots"), exist_ok=True)
        open(os.path.join(priv, "shots", "settings-es-1.ppm"), "wb").write(b"P6\n2 1\n255\n" + bytes([10, 20, 30, 40, 50, 60]))
        if os.path.exists(os.path.join(priv, "np-settings-open")): os.remove(os.path.join(priv, "np-settings-open"))
    if key.startswith("look"):
        os.makedirs(os.path.join(priv, "shots"), exist_ok=True)
        for n in (500, 560):
            open(os.path.join(priv, "shots", "shot_%06d.png" % n), "wb").write(b"\x89PNG fake")
    print("launched")
else:
    print("ok")
'''


class FakeConsole(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        bindir = cls.tmp / "bin"
        bindir.mkdir()
        for name, text in (("curl", CURL), ("nc", NC)):
            (bindir / name).write_text(text)
            (bindir / name).chmod(0o755)
        cls.bin = bindir

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(dir=self.tmp))
        fs = self.root / "fs" / "ux0"
        (fs / "app" / "CDXMP0001").mkdir(parents=True)
        (fs / "app" / "MSCN00003").mkdir(parents=True)
        (fs / "app" / "MSCN00003" / "eboot.bin").write_bytes(b"installed port, never touched")
        (fs / "data" / "mscharged" / "game").mkdir(parents=True)
        priv = fs / "data" / "mscharged-perf-test"
        priv.mkdir(parents=True)
        (priv / "log.txt").write_text(S.gen_run("old", "tour", seed=99, build="old run"))       # an earlier run's log, still on the console
        self.logs = self.root / "logs"
        self.logs.mkdir()
        S.session(self.logs / "tmp")
        for rid in ("d1", "r11"):
            shutil.copy(self.logs / "tmp" / rid / "log.txt", self.logs / (rid + ".txt"))
        (self.logs / "lookbase.txt").write_text(S.gen_run("lookbase", "tour", seed=31))
        self.out = self.root / "out"
        self.env = dict(os.environ, PATH="%s:%s" % (self.bin, os.environ["PATH"]), FAKE_ROOT=str(self.root), FAKE_LOGS=str(self.logs), VITA_HOST="198.51.100.7",
                        OUT=str(self.out), FIRST_POLL_S="1", NAP="0", VERSIONS=str(self.root / "versions"))

    def sh(self, *args, **extra):
        env = dict(self.env, **extra)
        return subprocess.run(["bash", str(HERE / "session.sh"), *args], capture_output=True, text=True, env=env, timeout=120)

    def times(self):
        with open(self.out / "session-times.csv") as f:
            return list(csv.DictReader(f))

    def calls(self, kind=None):
        log = self.root / "curl.log"
        lines = log.read_text().splitlines() if log.exists() else []
        return [l for l in lines if kind is None or l.startswith(kind + " ")]

    def test_check_stage_run_report(self):
        r = self.sh("check")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("installed", r.stdout)
        r = self.sh("stage")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        r = self.sh("run", "d1")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        log = (self.out / "d1" / "log.txt").read_text()
        self.assertEqual(log, (self.logs / "d1.txt").read_text())          # cut at the last header: nothing of the old run
        self.assertIn("console-1.2 run d1", (self.root / "fs/ux0/data/mscharged-perf-test/np-player.txt").read_text())
        times = self.times()
        self.assertEqual(times[0]["id"], "d1")
        self.assertEqual(times[0]["status"], "done")
        again = self.sh("run", "d1")
        self.assertIn("skipped", again.stdout)
        self.assertEqual((self.root / "nc.log").read_text().count("launch CDXMP0001"), 1)
        rep = self.sh("report")
        self.assertIn("d1", rep.stdout)

    def test_clock_gate_500_and_444_logs(self):
        """clockfix17 F1: session.sh never stops on a clock. A normal run's log says 500 (or 494, or nothing asked at ~498) and passes silently; under 470 only warns; clock444 wants 444."""
        env444 = self.root / "env444.txt"; env444.write_text("MSC_VITA_CPU_MHZ=444\nMSC_VITA_CLOCK_DEFER=1\n")
        envmax = self.root / "envmax.txt"; envmax.write_text("MSC_VITA_CLOCK_POLICY=max\n")
        def log(name, text):
            f = self.root / name; f.write_text(text); return str(f)
        l500 = log("l500.txt", "[run] first frame: build x (test); clocks ARM 494 MHz, GPU 222 MHz\nclocks: policy max; requested ARM 500 rc 0x0, bus 222 rc 0x0, GPU 222 rc 0x0, xbar 166 rc 0x0; reported ARM 494 bus 222 GPU 222 xbar 166; measured 444 before, 497 after (MHz)\n")
        lplug = log("lplug.txt", "clocks: policy max; requested ARM none (the core already measures 498 MHz), bus 222 rc 0x0, GPU 222 rc 0x0, xbar 166 rc 0x0; reported ARM 333 bus 222 GPU 222 xbar 166; measured 498 before, 498 after (MHz)\n")
        l444 = log("l444.txt", "[run] first frame: build x (test); clocks ARM 444 MHz, GPU 222 MHz\nclocks: policy max (MSC_VITA_CPU_MHZ); requested ARM 444 rc 0x0, bus 222 rc 0x0, GPU 222 rc 0x0, xbar 166 rc 0x0; reported ARM 444 bus 222 GPU 222 xbar 166; measured 0 before, 444 after (MHz)\n")
        llow = log("llow.txt", "clocks: policy max; requested ARM 500 rc 0x802B0000, then 444 rc 0x0, bus 222 rc 0x0, GPU 222 rc 0x0, xbar 166 rc 0x0; reported ARM 444 bus 222 GPU 222 xbar 166; measured 444 before, 444 after (MHz)\n")
        lfall = log("lfall.txt", open(l500).read() + "clocks: the ARM core measures 333 MHz now (system reports 333)\n")
        for rid, envf, lg, warn in (("d1", envmax, l500, False), ("d1", envmax, lplug, False), ("d1", envmax, llow, True), ("cur-tour1", envmax, lfall, True),
                                    ("clock444", env444, l444, False), ("clock444", env444, l500, True), ("b-npipe-1", envmax, log("none.txt", "nothing\n"), True)):
            r = self.sh("clockcheck", rid, str(envf), lg)
            self.assertEqual(r.returncode, 0, rid + ": must never stop: " + r.stdout + r.stderr)
            self.assertEqual("WARNING" in r.stdout, warn, "%s %s: %s" % (rid, lg, r.stdout))
        self.assertIn("requested ARM 500", self.sh("clockcheck", "d1", str(envmax), l500).stdout)
        # the hard stop is gone from the script and the old 444-only demand with it
        script = (HERE / "session.sh").read_text()
        self.assertNotIn("ran at other clocks", script)

    def test_night_deadlines_skip_and_report(self):
        lst = self.root / "night.list"
        lst.write_text("night\t480\t480\nphase\tp0\t75\nrun\td1\ttour\t1\t1.3\tp0\nphase\tp1\t40\nrun\td2\ttour\t1\t1.3\tp1\nphase\tend\t480\n")
        state = self.out / "state"
        state.mkdir(parents=True)
        (state / "version").write_text("1.3\n")
        (state / "night-start").write_text(str(int(__import__("time").time()) - 50 * 60) + "\n")     # 50 minutes into the night
        r = self.sh("night", NIGHT_LIST=str(lst))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue((self.out / "d1" / "log.txt").is_file())
        self.assertFalse((self.out / "d2" / "log.txt").exists())                  # past the deadline of its phase
        self.assertIn("d2", (self.out / "skipped.txt").read_text())
        self.assertIn("night over", r.stdout + r.stderr)
        self.assertIn("d1", (self.out / "night-progress.txt").read_text())

    def test_swap_and_version_guard(self):
        v = self.root / "versions" / "1.1"
        (v / "shadercache" / "key").mkdir(parents=True)
        (v / "eboot.bin").write_bytes(b"SCE\0 fake 1.1")
        (v / "shadercache" / "key" / "a.bin").write_bytes(b"x")
        wrong = self.sh("run", "r11")
        self.assertNotEqual(wrong.returncode, 0 if (self.out / "state" / "version").exists() else 1000)
        r = self.sh("swap", "1.1")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual((self.root / "fs/ux0/app/CDXMP0001/eboot.bin").read_bytes(), b"SCE\0 fake 1.1")
        self.assertTrue((self.root / "fs/ux0/app/CDXMP0001/shadercache/key/a.bin").is_file())
        r = self.sh("run", "d2")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("needs version 1.3", r.stdout + r.stderr)
        r = self.sh("run", "r11")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_failed_swap_does_not_leave_the_old_version_recorded(self):
        v = self.root / "versions" / "1.1"
        (v / "shadercache" / "key").mkdir(parents=True)
        (v / "eboot.bin").write_bytes(b"SCE\0 fake 1.1")
        (v / "shadercache" / "key" / "a.bin").write_bytes(b"x")
        state = self.out / "state"
        state.mkdir(parents=True)
        (state / "version").write_text("1.3\n")
        r = self.sh("swap", "1.1", FAKE_FAIL_PUT="shadercache", FTP_TRIES="1")
        self.assertNotEqual(r.returncode, 0)                               # the upload broke half way: eboot.bin is 1.1's, the shader pack is not
        self.assertEqual((self.root / "fs/ux0/app/CDXMP0001/eboot.bin").read_bytes(), b"SCE\0 fake 1.1")
        self.assertNotEqual((state / "version").read_text().strip(), "1.3")   # the console is not "1.3" any more
        r = self.sh("run", "d1")
        self.assertNotEqual(r.returncode, 0)                               # no run on a mixed console
        self.assertIn("swap", r.stdout + r.stderr)

    def test_every_env_file_is_stamped_so_the_settings_guard_leaves_it(self):
        # the isolated test app runs the 1.3 stale-settings guard on the np-player.txt it finds: an unstamped file loses MSC_DYNRES=0, MSC_VITA_RENDER=... and so on
        files = sorted(HERE.glob("env/*.txt")) + sorted(HERE.glob("lan/*.txt"))
        self.assertGreater(len(files), 300)
        for f in files:
            head = f.read_text().splitlines()[:8]
            self.assertTrue(any(l.startswith("# np-player.txt written by 1.3.0") for l in head), f.name)

    def test_only_the_test_app_is_touched(self):
        self.sh("check")
        self.sh("stage")
        self.sh("run", "d1")
        for line in self.calls():
            kind, path = line.split(" ", 1)
            if kind in ("PUT", "DELE"):
                self.assertTrue(path.startswith("ux0:/data/mscharged-perf-test/") or path.startswith("ux0:/app/CDXMP0001/"), line)
        self.assertNotIn("MSCN00003", "".join(l for l in self.calls() if l.startswith(("PUT", "DELE", "GET"))))
        self.assertNotIn("MSCN00003", (self.root / "nc.log").read_text())
        self.assertEqual((self.root / "fs/ux0/app/MSCN00003/eboot.bin").read_bytes(), b"installed port, never touched")
        for line in (self.root / "nc.log").read_text().splitlines():
            self.assertIn(line, {"version", "help", "quit CDXMP0001", "launch CDXMP0001"})

    def test_driver_only_accepts_targeted_quit(self):
        script = (HERE / "session.sh").read_text()
        guard = script[script.index("\nvcmd() {"):script.index("\nneed_host() {")]
        probe = 'die() { echo "$*" >&2; exit 1; }\nTID=CDXMP0001; H=fake; CMD_PORT=1338\n' + guard + '\nvcmd "$1"'
        commands = ("quit CDXMP0001", "quit all", "quit MSCN00003", "quit OTHER0001", "quit", "quit CDXMP0001 extra",
                    "quit CDXMP0001\nquit all")
        for command in commands:
            with self.subTest(command=command):
                result = subprocess.run(["bash", "-c", probe, "guard", command], capture_output=True, text=True, env=self.env)
                if command == "quit CDXMP0001":
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                else:
                    self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("refusing command", result.stdout + result.stderr)
        self.assertEqual((self.root / "nc.log").read_text().splitlines(), ["quit CDXMP0001"])

    def test_driver_refuses_other_paths_and_commands(self):
        script = (HERE / "session.sh").read_text()
        for expect in ("remote_ok", "refusing to write", "refusing command"):
            self.assertIn(expect, script)
        # macOS Bash 3.2 can source an empty process-substitution stream here.
        guards = script[script.index("\nnorm() {"):script.index("\nfurl() {")]
        probe = subprocess.run(["bash", "-c", guards + '\nPRIV=ux0:/data/mscharged-perf-test; APP=ux0:/app/CDXMP0001; '
                                'for p in ux0:data/mscharged-perf-test/log.txt ux0:/app/CDXMP0001/eboot.bin ux0:/app/CDXMP0001/shadercache/a/b; do remote_ok $p && echo ok; done; '
                                'for p in ux0:data/mscharged/log.txt ux0:/app/MSCN00003/eboot.bin ux0:/app/CDXMP0001/sce_sys/param.sfo ux0:data/mscharged-perf-test/../mscharged/x ux0:/data/mscharged-perf-testx/y; do remote_ok $p && echo BAD; done; true'],
                               capture_output=True, text=True)
        self.assertEqual(probe.stdout.split(), ["ok", "ok", "ok"], probe.stdout + probe.stderr)

    def test_look_run_pulls_screenshots(self):
        r = self.sh("run", "lookbase")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(sorted(p.name for p in (self.out / "lookbase" / "shots").iterdir()), ["shot_000500.png", "shot_000560.png"])

    def test_a_run_that_never_ends_is_reported(self):
        (self.logs / "d1.txt").write_text(S.gen_run("d1", "tour", seed=11).replace("[plat] script exit", "[plat] no exit"))
        r = self.sh("run", "d1", FIRST_POLL_S="1", POLL_S="1", STALL_S="3")
        self.assertNotEqual(r.returncode, 0)
        times = self.times()
        self.assertNotEqual(times[0]["status"], "done")


    def test_settings_run_opens_the_screen_and_pulls_its_pictures(self):
        (self.logs / "cur-setes.txt").write_text("[vita] ===== Mario Strikers Charged, PS Vita native port, build synthetic =====\n"
                                                 "[settings] test script finished\n[vita] settings screen done: quitting\n")
        r = self.sh("run", "cur-setes")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("PUT ux0:/data/mscharged-perf-test/np-settings-open", self.calls("PUT"))
        self.assertEqual([p.name for p in (self.out / "cur-setes" / "shots").iterdir()], ["settings-es-1.ppm"])
        self.assertEqual((self.out / "cur-setes" / "status.txt").read_text().strip(), "done")
        looks = load(HERE / "looks.py", "looks_core")
        text, n = looks.core_page(self.out)
        self.assertIn("settings-es-1.png", text)
        self.assertEqual((self.out / "cur-setes" / "shots" / "settings-es-1.png").read_bytes()[:8], b"\x89PNG\r\n\x1a\n")

    def test_a_list_never_stops_at_a_bad_step(self):
        # a swap that fails skips its version's runs, a run that never ends is logged and the next one still starts
        (self.logs / "cur-tour1.txt").write_text(S.gen_run("cur-tour1", "ctour", seed=5).replace("[plat] script exit", "[plat] no exit"))
        (self.logs / "cur-pipe1.txt").write_text(S.gen_run("cur-pipe1", "cpipe", seed=6))
        plan = self.root / "my.list"
        plan.write_text("run\tcur-tour1\tctour\t4.4\t1.3\nswap\t1.1\nrun\tv11-tour\tctour\t5.4\t1.1\nswap\t1.3\nrun\tcur-pipe1\tcpipe\t2.3\t1.3\n")
        v = self.root / "versions" / "1.3"
        (v / "shadercache").mkdir(parents=True)
        (v / "eboot.bin").write_bytes(b"SCE\0 fake 1.3")
        (v / "shadercache" / "a.bin").write_bytes(b"x")
        r = self.sh("list", str(plan), FIRST_POLL_S="1", POLL_S="1", STALL_S="2")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("swap to 1.1 failed", r.stdout)
        self.assertIn("v11-tour skipped", r.stdout)
        rows = {row["id"]: row["status"] for row in self.times()}
        self.assertEqual(rows["cur-tour1"], "stalled")
        self.assertEqual(rows["swap-1.1"], "failed")
        self.assertEqual(rows["cur-pipe1"], "done")
        self.assertNotIn("v11-tour", rows)


class Core(unittest.TestCase):
    """The core session (matrix.py tier 0): what it holds, its order against drift, its time budget, and what analyse.py / issues.py make of it."""

    def core(self):
        return [r for r in M.RUNS if r["tier"] == 0]

    def test_composition(self):
        core = self.core()
        perf = [r for r in core if r["role"] in ("cref", "core")]
        have = {(r["version"], r["route"]) for r in perf}
        for route in ("ctour", "cpipe", "clava", "mega", "crep"):          # every scene: 1.1 against 1.3 current
            self.assertIn(("1.1", route), have)
            self.assertIn(("1.3", route), have)
        for route in ("ctour", "cpipe", "clava", "mega"):                  # the proposed defaults on the tour, Pipeline Central, items and Mega
            self.assertIn(("1.3p", route), have)
        looks = {(r["version"], r["id"].split("-", 1)[1]) for r in core if r["role"] == "look"}
        for kind in ("look", "lookpipe", "lookmega", "lookrep", "looksmall"):
            self.assertIn(("1.1", kind), looks)
            self.assertIn(("1.3", kind), looks)
        for kind in ("look", "lookpipe", "lookmega"):
            self.assertIn(("1.3p", kind), looks)
        self.assertIn(("1.3", "lookes"), looks)
        by_id = {r["id"]: r for r in core}
        self.assertEqual(dict(by_id["cur-lookes"]["env"])["MSC_LANGUAGE"], "es")
        self.assertEqual(dict(by_id["cur-looksmall"]["env"])["MSC_VITA_RENDER"], M.SMALL)
        self.assertIn("shot:", by_id["cur-setes"]["settings"])
        self.assertTrue(by_id["cur-setes"]["settings"].endswith("circle"))
        for r in core:
            if r["route"] == "clava":
                self.assertEqual(dict(r["env"]), M.LAVA)
        self.assertIn("powerup=10", (HERE / "routes" / "clava.txt").read_text())
        self.assertIn("stadium=7 ", (HERE / "routes" / "clava.txt").read_text())
        # the files: perf runs log the hitches, looks are held at 30 and take pictures, nothing else differs between versions of one route
        for r in core:
            text = (HERE / "env" / (r["id"] + ".txt")).read_text()
            if r["role"] in ("cref", "core"):
                self.assertIn("MSC_HITCH_MS=50", text)
                self.assertNotIn("MSC_VITA_SHOT", text)
            if r["role"] == "look":
                self.assertIn("MSC_FPS=30", text)
                self.assertIn("MSC_VITA_SHOT=", text)
                self.assertNotIn("MSC_HITCH_MS", text)
        for route in ("ctour", "cpipe", "clava", "mega", "crep"):
            envs = {tuple(r["env"]) for r in core if r["route"] == route and r["role"] in ("cref", "core")}
            self.assertEqual(len(envs), 1, route)

    def test_order_controls_drift(self):
        steps = M.session_order(0)
        self.assertEqual([i for k, i in steps if k == "swap"], ["1.3p", "1.3", "1.1", "1.3"])
        order = [i["id"] for k, i in steps if k == "run"]
        tours = [order.index(i) for i in ("cur-tour1", "cur-tour2", "cur-tour3")]
        pipes = [order.index(i) for i in ("cur-pipe1", "cur-pipe2", "cur-pipe3")]
        self.assertTrue(tours[0] < order.index("prop-tour") < tours[1] < order.index("v11-tour") < tours[2])     # references at the start, middle and end
        self.assertTrue(pipes[0] < order.index("prop-pipe") < pipes[1] < order.index("v11-pipe") < pipes[2])
        for route in ("clava", "mega", "crep"):     # each 1.1 run between two current references of its route
            refs = [order.index(r["id"]) for r in M.RUNS if r["tier"] == 0 and r["role"] == "cref" and r["route"] == route]
            v11 = order.index([r["id"] for r in M.RUNS if r["tier"] == 0 and r["version"] == "1.1" and r["route"] == route and r["role"] == "core"][0])
            self.assertTrue(min(refs) < v11 < max(refs), route)
        # the core comes first in `all` and in `full`, and `full` goes tier by tier
        full = [i for k, i in M.session_order(3, staged=True) if k == "run"]
        self.assertEqual([r["id"] for r in full[:len(order)]], order)
        self.assertEqual([r["tier"] for r in full], sorted(r["tier"] for r in full))
        self.assertNotIn("clk500-tour", [r["id"] for r in full])               # (a plug-in set by hand: `addon` only)
        self.assertEqual([i["id"] for k, i in M.session_order(1) if k == "run"][:len(order)], order)

    def test_time_budget(self):
        blocks = M.core_blocks()
        total = sum(b[1] for b in blocks)
        self.assertAlmostEqual(total, M.estimate(0, False)[0], places=6)
        self.assertLessEqual(total, M.CORE_BUDGET_MIN)
        self.assertGreaterEqual(total, 75.0)
        self.assertEqual([b[0][0] for b in blocks[1:]], ["A", "B", "C", "D", "E"])
        r = run(sys.executable, HERE / "gen_kit.py", "--plan", "--tier", "0")
        self.assertEqual(r.returncode, 0, r.stderr)
        for name, minutes, _ in blocks:
            self.assertIn("| %s | %.1f |" % (name, minutes), r.stdout)
        self.assertIn("Core total %.1f min" % total, r.stdout)

    def test_analysis_and_issues(self):
        issues = load(HERE / "issues.py", "issues")
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            S.core_session(d)
            out = io.StringIO()
            rows = {row["scene"]: row for row in A.core_summary(d, out=out)}
            self.assertEqual(rows["play"]["vs11"][1], "better")
            self.assertEqual(rows["pipe-play"]["vsprop"][1], "WORSE")
            self.assertEqual(rows["play"]["1.3"]["size"], (960, 544))
            self.assertEqual(rows["play"]["1.1"]["size"], (640, 448))
            self.assertEqual(rows["pipe-play"]["1.3"]["gpu_busy"], 36.0)
            self.assertEqual(rows["play"]["refs"], 3)
            self.assertIn("HEAP 95% of the block", out.getvalue())
            # a picture with a dark last column, and a flat one
            shots = d / "cur-looksmall" / "shots"
            shots.mkdir(parents=True)
            looks = load(HERE / "looks.py", "looks_png")
            w, h = 40, 30
            def img(fn):
                return bytes(v for y in range(h) for x in range(w) for v in fn(x, y))
            (shots / "shot_000700.png").write_bytes(looks.png_bytes(w, h, img(lambda x, y: (10, 10, 10) if x == w - 1 else ((100 + 3 * y) % 256,) * 3)))
            (shots / "shot_000800.png").write_bytes(looks.png_bytes(w, h, img(lambda x, y: (0, 0, 0))))
            self.assertEqual([k for k, _ in issues.picture_checks(shots / "shot_000700.png")], ["edge"])
            self.assertEqual([k for k, _ in issues.picture_checks(shots / "shot_000800.png")], ["blank"])
            findings, verdicts, core_rows = issues.report(d, d / "ISSUES.md")
            text = (d / "ISSUES.md").read_text()
            for sev in issues.SEVERITIES:
                self.assertIn("### " + sev, text)
            self.assertIn("| pipe-play |", text)
            self.assertEqual({f["run"] for f in findings["Crash or hang"]}, {"v11-rep"})
            self.assertEqual({f["run"] for f in findings["Heap near its limit"]}, {"cur-mega2"})
            self.assertEqual({f["run"] for f in findings["Shader compiles during play"]}, {"cur-pipe1"})
            self.assertIn("prop-pipe", {f["run"] for f in findings["Performance regressions"]})
            self.assertEqual({f["scene"] for f in findings["Picture faults"]}, {"shot_000700.png", "shot_000800.png"})

    def test_switch_verdicts(self):
        issues = load(HERE / "issues.py", "issues2")
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            S.session(d)
            findings, verdicts, _ = issues.report(d, d / "ISSUES.md")
            v = {rid: (verdict, row) for rid, _, verdict, _, row in verdicts}
            self.assertEqual(v["msaa0"], ("KEEP DEFAULT", "-"))                      # off is slower: the default helps (set18: ANTI-ALIASING left the menu, no row owns the key)
            self.assertTrue(v["fxn"][0].startswith("FLIP DEFAULT"))                   # the tested value is faster
            self.assertEqual(v["fxn"][1], "-")                                      # (1.3 prune: EFFECTS QUALITY left the screen, no row owns the key)
            self.assertTrue(v["timing0"][0].startswith("no effect"))
            self.assertIn("## Switch verdicts", (d / "ISSUES.md").read_text())


class Looks(unittest.TestCase):
    def test_contact_sheet(self):
        looks = load(HERE / "looks.py", "looks")
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            for rid, seed in (("lookbase", 1), ("lookmsaa0", 2)):
                (d / rid / "shots").mkdir(parents=True)
                (d / rid / "log.txt").write_text(S.gen_run(rid, "tour", seed=seed))
                for n in (450, 1500, 2400):
                    (d / rid / "shots" / ("shot_%06d.png" % n)).write_bytes(b"png")
            (d / "lookmsaa0" / "shots" / "shot_002400.png").unlink()
            runs, order = looks.collect(d, "lookbase")
            self.assertEqual(order, ["lookbase", "lookmsaa0"])
            text = looks.page(d, runs, order, "lookbase")
            self.assertIn("lookbase/shots/shot_001500.png", text)
            self.assertIn("no picture", text)
            self.assertIn("walkout", text)           # present 2400 of the synthetic tour is in the walk-out



# ---- the night -------------------------------------------------------------------------------------------------------------------------

class Night(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        S.night_session(cls.tmp)
        r = run(sys.executable, HERE / "issues.py", cls.tmp, "--morning", "--pixels")
        assert r.returncode == 0, r.stdout + r.stderr
        cls.text = (cls.tmp / "MORNING.md").read_text()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_plan_fits_the_night_and_phases_keep_their_deadlines(self):
        for pending in (False, True):
            est = N.estimate(with_pending=pending)
            total = est[0] if isinstance(est, tuple) else est
            self.assertLessEqual(total, N.BUDGET_MIN if not pending else N.BUDGET_MIN + 5, "the night is longer than its budget")
        steps, dropped = N.schedule()
        for rid, miss in dropped:
            for key, branch in miss:
                self.assertIn(key, N.PENDING)

    def test_every_night_run_has_env_and_route(self):
        routes = set(R.FILES) if hasattr(R, "FILES") else set()
        for r in N.RUNS:
            self.assertTrue((HERE / "env" / (r["id"] + ".txt")).is_file(), r["id"])
            if routes:
                self.assertIn(r["route"] + ".txt", routes, r["id"])

    def test_stability_routes_cover_every_stadium_and_end(self):
        import gen_routes as G
        self.assertEqual(len(G.STAB_STADIUMS), 12)
        self.assertEqual(set(G.STAB_STADIUMS) | set(G.INTRO_STADIUMS), set(range(16)))
        files = G.FILES
        self.assertEqual(files["cstab.txt"]().count("launch "), 12)
        self.assertEqual(files["cintro.txt"]().count("launch "), 8)
        self.assertEqual(files["cend.txt"]().count("launch "), 3)
        self.assertIn("exit", files["cend.txt"]().splitlines()[-1])

    def test_morning_names_the_crashes(self):
        t = self.text
        self.assertIn("## 1. What crashed", t)
        self.assertIn("o-stab", t)
        self.assertIn("psp2core-2026_11_12-02_14_00-eboot.bin.psp2dmp", t)
        self.assertIn("s-intro", t)
        self.assertIn("12 of 12 matches ended", t)
        self.assertIn("Sleep and resume were not tried", t)

    def test_morning_fps_table_has_the_three_modes_and_noise(self):
        t = self.text
        self.assertIn("1.1", t); self.assertIn("LOCKED 60", t)
        self.assertIn("Pipeline Central play", t)
        self.assertIn("same-night noise", t)
        self.assertIn("holds 60", t)

    def test_morning_verdicts(self):
        t = self.text
        row = lambda rid: next(l for l in t.splitlines() if l.startswith("| %s " % rid))
        self.assertIn("ADOPT (faster", row("n-fxtight"))
        self.assertIn("LEAVE OFF", row("n-fxgov"))
        self.assertIn("NO GAIN", row("n-memfree2"))
        self.assertIn("KEEP ON", row("n-gpubusy0"))
        self.assertIn("FLIP OFF", row("n-memfree0"))
        self.assertIn("[marginal", row("n-viewlayers"))
        self.assertIn("INCONCLUSIVE", row("n-hsr"))
        self.assertIn("BROKEN", row("n-finclamp2"))
        self.assertIn("ADOPT IF THE PICTURES AGREE", row("n-texfull"))

    def test_morning_proposed_defaults_and_not_run(self):
        t = self.text
        self.assertIn("MSC_FX_TIGHT=8   # n-fxtight", t)
        self.assertNotIn("# n-fxgov", t.split("## 5.")[1].split("## 6.")[0])
        self.assertIn("n-quadfast", t.split("## 6.")[1])
        self.assertIn("n-passdown", t.split("## 6.")[1])

    def test_warm_up_records_are_dropped_but_a_short_stretch_is_kept(self):
        none = A.analyse(self.tmp, warm=0)
        some = A.analyse(self.tmp, warm=2)
        self.assertTrue(none and some)
        stats = A.night_stats(self.tmp, warm=2)
        self.assertTrue(stats)


class Versions(unittest.TestCase):
    def test_prepare_versions(self):
        tests = load(REPO / "tests/test_hardware_stage.py", "stage_tests")
        tool = load(REPO / "tools/stage-hardware-test.py", "stage_tool2")
        prep = load(HERE / "prepare-versions.py", "prep")
        with tempfile.TemporaryDirectory() as d:
            d = Path(d).resolve()
            eboot = b"SCE\0" + b"\x01" * 40
            vpk = d / "t.vpk"
            with zipfile.ZipFile(vpk, "w") as z:
                z.writestr("eboot.bin", eboot)
                z.writestr("sce_sys/param.sfo", tests.sfo())
                z.writestr("shadercache/key1/a.bin", b"aa")
            m = prep.prepare(tool, "1.1", vpk, d / "versions", tool.sha(eboot))
            self.assertEqual(m["eboot_sha256"], tool.sha(eboot))
            self.assertEqual((d / "versions/1.1/shadercache/key1/a.bin").read_bytes(), b"aa")
            self.assertEqual((d / "versions/1.1/eboot.bin").read_bytes(), eboot)
            with self.assertRaises(ValueError):
                prep.prepare(tool, "1.1", vpk, d / "versions")                      # exists
            with self.assertRaises(ValueError):
                prep.prepare(tool, "1.0", vpk, d / "versions", "0" * 64)            # wrong digest
            bad = d / "bad.vpk"
            with zipfile.ZipFile(bad, "w") as z:
                z.writestr("eboot.bin", eboot)
                z.writestr("sce_sys/param.sfo", tests.sfo(title="MSCN00003", content="EP9000-MSCN00003_00-HARDWARETEST0000"))
                z.writestr("shadercache/k/a.bin", b"aa")
            with self.assertRaises(ValueError):
                prep.prepare(tool, "1.2", bad, d / "versions2")                     # the installed port's identity


class Sitting(unittest.TestCase):
    """The r15 consolidated sitting (sitting.py, r17-sitting.txt) and the fast-boot variants (env-fb/)."""

    def test_fastboot_env_and_routes(self):
        import re as _re
        n = 0
        for r in M.RUNS:
            fb = M.fastboot_route(r)
            p = HERE / "env-fb" / (r["id"] + ".txt")
            self.assertEqual(p.is_file(), bool(fb), r["id"])
            if not fb:
                continue
            n += 1
            text = p.read_text()
            self.assertIn("MSC_INPUT_SCRIPT=ux0:data/mscharged-perf-test/%s.txt" % fb, text)
            self.assertEqual(text.count("MSC_INPUT_SCRIPT="), 1)
            self.assertTrue((HERE / "fastboot" / "routes" / (fb + ".txt")).is_file())
            self.assertNotIn("320x224", text)     # (the emulator's render size is in fastboot/env/, never in a console env file)
        self.assertGreater(n, 100)
        # the bootmatch spec is the old route's launch line (stadium:home:away and its keys)
        for fb, src in (("fb-dump", "worstgpu"), ("fb-pipe", "cpipe"), ("fb-lava", "clava"), ("fb-replay", "replay"), ("fb-crep", "crep")):
            old = (HERE / "routes" / (src + ".txt")).read_text()
            launch = dict(t.split("=", 1) for t in _re.search(r"^launch (.*)$", old, _re.M).group(1).split())
            new = (HERE / "fastboot" / "routes" / (fb + ".txt")).read_text()
            spec = _re.search(r"^bootmatch (\S+)(.*)$", new, _re.M)
            self.assertEqual(spec.group(1), "%s:%s:%s" % (launch["stadium"], launch["home"], launch["away"]), fb)
            for k in ("hsk", "ask", "time", "skill", "powerup"):
                if k in launch:
                    self.assertIn("%s=%s" % (k, launch[k]), spec.group(2), fb + " " + k)
        self.assertTrue((HERE / "fastboot" / "routes" / "fb-tour.txt").read_text().find("\nbootmatch vice:3:6") >= 0)   # the tour's menus keep the default stadium Vice (13), captains 3 and 6

    def test_sitting_list(self):
        import sitting as S
        by = {r["id"]: r for r in M.RUNS}
        ids = [p for k, p in S.steps() if k == "run"]
        xids = [p for k, p in S.extra_steps() if k == "run"]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertEqual(len(xids), len(set(xids)))
        self.assertFalse(set(ids) & set(xids), "a run is in both lists")
        for i in ids + xids:
            self.assertIn(i, by)
            self.assertTrue((HERE / "env" / (i + ".txt")).is_file())
        pos = {i: n for n, i in enumerate(ids)}
        # block A: the 60 answer first, after the two references; the simt gate before the other simt runs; the stutter fixes after
        order = ["d1", "worstgpu1", "dlweld0", "lock60tour", "lock60pipe", "lock60dump", "lock60lava", "warboff-l", "live19-leave", "scene18-pause", "lock60gs-v", "lock60gs-d", "simt-blk-v", "simt-v", "crep1", "nodepth1", "heldaa13w2", "cutlearn2"]
        for a, b in zip(order, order[1:]):
            self.assertLess(pos[a], pos[b], (a, b))
        self.assertLess(pos["pgo-base-d1-a"], pos["pgo-on-lock60"])
        self.assertLess(pos["pgo-on-lock60"], pos["simt-pgo"])
        xpos = {i: n for n, i in enumerate(xids)}
        self.assertLess(xpos["pgox-d1-a"], xpos["live19-shake"])
        self.assertLess(xpos["simt-d"], xpos["simt-lava"])                  # simt-d moved to X3 (the settings screens took its place in block A)
        self.assertLess(xpos["dg20-s6-blend-d"], xpos["rainoff-d"])         # the rain hunt after S6, the gpuscene20 diagnostics last in X4 (47 emulator faults with them on)
        self.assertLess(xpos["dg20-s14-up-d"], xpos["scenes-d"])
        self.assertLess(xpos["scenes-d"], xpos["scenes-v"])
        self.assertNotIn("ainoclockoff", by)
        self.assertLess(xpos["exactall-vice"], xpos["lock60x-dump"])       # the combined GPU levers only after the exact switches ran with their verify
        self.assertLess(xpos["simt-lava"], xpos["simt-c1"])
        self.assertLess(xpos["cutsshapes1"], xpos["cutsshapes1-2"])         # (the old KeyError: it moved to the extra list, so it is looked up there)
        self.assertLess(xpos["v5h3"], 10 ** 6)
        # alternating executables: the plain executable is back at the end of B and of every extra unit that swaps; three PGO eswaps in B
        for steps in (S.steps(), S.extra_steps()):
            es = [p for k, p in steps if k == "eswap"]
            self.assertEqual(es[-1], S.PKG_BASE)
        es = [p for k, p in S.steps() if k == "eswap"]
        self.assertEqual(es.count(S.PKG_PGO), 2)
        for scn in ("d1", "wg"):
            for side in ("base", "on"):
                self.assertEqual(sum(1 for i in ids if i.startswith("pgo-%s-%s-" % (side, scn))), 2)
        # budgets (estimates): block A about 2 h, A + B the natural stop at 3 h 15 at most
        t = S.totals()
        self.assertLessEqual(t["A"], 140.0)
        self.assertLessEqual(t["A"] + t["B"], 195.0)
        self.assertEqual((HERE / "r17-sitting.txt").read_text(), S.sitting_text())
        self.assertEqual((HERE / "r17-sitting-extra.txt").read_text(), S.sitting_text(extra=True))
        self.assertEqual((HERE / "r17-lan.txt").read_text(), S.lan_text())
        # no run of the main list may need another version than the console has
        self.assertEqual({by[i]["version"] for i in ids}, {"1.3"})
        # every unattended list ends with the owner's settings restored
        self.assertEqual([k for k, _ in S.steps()][-2:], ["restore", "stop"])
        self.assertEqual([k for k, _ in S.extra_steps()][-2:], ["restore", "stop"])

    def test_simt_runs_are_valid_only_with_the_log_key(self):
        """Fable verify10 P5: every simt-* run carries MSC_OVERLAP_LOG=1, and the overlap runs the renderrng and the simfix20 note."""
        simt = [r for r in M.RUNS if r["id"].startswith("simt-") or r["id"] == "simt30-d"]
        self.assertGreaterEqual(len(simt), 11)
        for r in simt:
            self.assertIn(("MSC_OVERLAP_LOG", "1"), r["env"], r["id"])
            self.assertIn("simfix20", r["why"], r["id"])
            if r["id"] != "simt-blk-v":
                self.assertIn(("MSC_RENDER_RNG", "2"), r["env"], r["id"])
                self.assertIn(("MSC_SIMT", "2"), r["env"], r["id"])
        self.assertEqual(dict(next(r for r in simt if r["id"] == "simt-blk-v")["env"])["MSC_SIMT"], "1")
        for leg in M.LAN_LEGS:
            if leg["id"].startswith("lan-simt"):
                self.assertIn(("MSC_OVERLAP_LOG", "1"), leg["host_env"])

    def test_lock60_runs_carry_the_head_line(self):
        for r in M.RUNS:
            if r["id"].startswith("lock60") and r["id"] != "lock60x-dump" and r["id"] != "lock60x-lava":
                self.assertIn(("MSC_HEAD_STAT", "1"), r["env"], r["id"])
        for i in ("lock60tour", "lock60pipe", "lock60dump", "lock60lava"):
            self.assertIn("split", next(r for r in M.RUNS if r["id"] == i)["why"])

    def test_new_routes_exist_and_bootmatch(self):
        import re as _re
        for fb, stadium in (("fb-sand", "15"), ("fb-live19", "13"), ("fb-live19-crowd", "13"), ("fb-live19-rear", "13"), ("fb-live19-leave", "13")):
            t = (HERE / "fastboot" / "routes" / (fb + ".txt")).read_text()
            self.assertTrue(_re.search(r"^bootmatch %s:" % stadium, t, _re.M), fb)
            self.assertIn("log STATE", t) if fb.startswith("fb-live19") else self.assertIn("@begin sand-play", t)


class Online24(unittest.TestCase):
    """Unit ON of the extra list (online24): the routes, the env files, the pass rules, the Settings key words."""

    def runs(self):
        import gen_online as G
        return G, {r["id"]: r for r in M.RUNS}

    def test_env_files(self):
        G, by = self.runs()
        for sp in G.RUN_SPECS:
            env = dict(by[sp["id"]]["env"])
            self.assertEqual(env["MSC_ONLINE_HOST"], "old" if sp["id"] == "on-hub-old" else "auto", sp["id"])   # every run names the address rule; the stock default is old
            self.assertEqual(env["MSC_NET_REQLOG"], "1", sp["id"])
            self.assertEqual(env["MSC_ONLINE_NAME"], G.NAME, sp["id"])                        # a preset name: the system keyboard is never opened
            self.assertNotIn("MSC_ONLINE_URL", env, sp["id"])                                    # the isolated build's reader refuses a value with / or :
        self.assertEqual(dict(by["on-quick"]["env"])["MSC_ONLINE_FRIENDS"], "1")                 # FRIENDS / PLAY WITH ANYONE are not offered by the tree itself
        self.assertTrue(G.NAME.isalnum() and 2 <= len(G.NAME) <= 10)

    def test_routes_log_every_state_and_are_current(self):
        G, _ = self.runs()
        for key, (name, fn, states, mins) in G.ROUTES.items():
            text = (HERE / "routes" / name).read_text()
            self.assertEqual(text, fn(), name + " is out of date: python3 gen_routes.py")
            for st in states:
                self.assertRegex(text, r"(?m)^log STATE %s$" % re.escape(st), "%s: %s" % (name, st))
            self.assertTrue(text.rstrip().endswith("exit"), name)
            self.assertNotIn("MSC_ONLINE_URL", "\n".join(l for l in text.splitlines() if not l.startswith("#")))

    def test_unit_is_first_and_queueable(self):
        text = (HERE / "r17-sitting-extra.txt").read_text()
        heads = [l for l in text.splitlines() if l.startswith("# ==== ")]
        self.assertTrue(heads[0].startswith("# ==== X:") and heads[1].startswith("# ==== ON:"))
        ids = []
        on = False
        for l in text.splitlines():
            if l.startswith("# ==== "):
                on = l.startswith("# ==== ON:")
            elif on and l.startswith("run\t"):
                ids.append(l.split("\t")[1])
        self.assertEqual(ids, ["on-hub", "on-hub-old", "on-lists", "on-quick", "on-ranked", "on-report"])
        mins = sum(M.minutes(next(r for r in M.RUNS if r["id"] == i)) + 0.4 for i in ids)
        self.assertLess(mins, 16.0)

    def test_settings_words_reach_save_a_report(self):
        """the pixel screen is Vita-only: the key words are walked over the page table of np_settings.cpp (page ONLINE, row of SAVE A REPORT)"""
        G, _ = self.runs()
        src = (REPO / "native/platform/vita/src/np_settings.cpp").read_text()
        table = src[src.index("constexpr PageTable kPages[]"):]
        table = table[:table.index("};")]
        pages = re.findall(r'\{"([A-Z ]+)", \{([^}]*)\}\}', table)
        names = [p for p, _ in pages]
        page = names.index("ONLINE")
        row = [r.strip().strip('"') for r in pages[page][1].split(",")].index("SAVE A REPORT")
        words = G.SETTINGS_REPORT.split()
        self.assertEqual(sum(w == "rtrigger" for w in words), page)
        self.assertEqual(sum(w == "down" for w in words), row)
        self.assertLess(words.index("cross"), words.index("circle"))
        self.assertEqual(words.count("cross"), 1)

    def test_pass_rules_self_test(self):
        r = subprocess.run([sys.executable, str(HERE / "online_check.py"), "--self-test"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_request_log_header_never_prints_ids(self):
        """np_reqlog.h: a segment with a digit or a capital is :id; `code` is read only from an error answer (a success body's code is the public code)"""
        cxx = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
        if not cxx:
            self.skipTest("no C++ compiler")
        tmp = Path(tempfile.mkdtemp())
        try:
            (tmp / "t.cpp").write_text(r'''#include "platform/sdk/net/online/np_reqlog.h"
#include <assert.h>
using namespace NpNet::ReqLog;
int main() {
    assert(SafePath("https://x.test/v1/rooms/K7Q-M4D/poll?v=3&token=abc") == "/v1/rooms/:id/poll");
    assert(SafePath("https://x.test/v1/matches/9f3a2b/result") == "/v1/matches/:id/result");
    assert(SafePath("https://x.test/v1/ranked/queue") == "/v1/ranked/queue");
    assert(SafePath("https://x.test") == "/");
    const char* ok = "{\"code\":\"ABCD-EFGH\",\"pollScale\":1.25}";
    const char* er = "{\"error\":{\"code\":\"over_budget\",\"level\":\"new-over\",\"retryAfterSec\":15}}";
    assert(Word(ok, strlen(ok), "pollScale") == "1.25");
    assert(Word(er, strlen(er), "code") == "over_budget" && Word(er, strlen(er), "level") == "new-over" && Word(er, strlen(er), "retryAfterSec") == "15");
    assert(HostLabel("https://strikers.psvita.dev/v1/info") == std::string("new"));
    assert(HostLabel("https://msc-online.cboc-vita.workers.dev/v1/info") == std::string("old"));
    assert(HostLabel("http://127.0.0.1:8787/v1/info") == std::string("other"));
    setenv("MSC_NET_REQLOG", "1", 1);
    Line("GET", "https://strikers.psvita.dev/v1/info", 200, 0, 0, 12, 0, ok, strlen(ok));   // must not print the public code (checked on stderr by the caller)
    return 0;
}
''')
            r = subprocess.run([cxx, "-std=c++11", "-I", str(REPO / "native"), "-I", str(REPO / "native/platform/sdk/net/online/.."), str(tmp / "t.cpp"), "-o", str(tmp / "t")], capture_output=True, text=True)
            if r.returncode != 0:
                self.skipTest("test does not compile here: " + r.stderr[-300:])
            r = subprocess.run([str(tmp / "t")], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("-> 200 12 ms reused=0 pollScale=1.25", r.stderr)
            self.assertNotIn("ABCD", r.stderr)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
