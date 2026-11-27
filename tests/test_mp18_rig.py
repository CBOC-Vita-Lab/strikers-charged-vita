"""mp18 (23 Nov): the multiplayer proof rig and the single-Vita console runs.

* tools/mp/lan/lan_join*.txt: the joiner waits until the host's game is LISTED (`wait found >= 1`) before it presses Join. The old fixed `idle 400` raced the host when the two
  consoles ran at different frame rates (LOCKED 60 against AUTO: the joiner's 400 frames were 6.7 s while the host had not created its game yet, Join hit an empty list and the
  pair timed out in the lobby: /home/claude/work/mp18-pairs/lan-plan-race1).
* tools/mp/online_pair.py --service-url: a service that is already running LOCALLY (the real Worker under `wrangler dev`), loopback only.
* tools/console-1.2: the MP unit (gen_online.py routes on-hostwait / lan-*-wait, MP18_RULES, online_check.py rules for mp-*).
"""
import importlib.util
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
LAN = REPO / "native" / "tools" / "mp" / "lan"
KIT = REPO / "tools" / "console-1.2"
sys.path.insert(0, str(KIT))


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(Path(path).parent))
    spec.loader.exec_module(mod)
    return mod


class JoinScripts(unittest.TestCase):
    def test_every_lan_join_script_waits_for_a_listed_game_before_join(self):
        files = sorted(LAN.glob("lan_join*.txt"))
        self.assertGreaterEqual(len(files), 9)
        for f in files:
            lines = [ln.strip() for ln in f.read_text().splitlines()]
            if "log joining" not in lines:
                continue
            j = lines.index("log joining")
            lobby = max(i for i in range(j) if lines[i] == "log STATE lobby")
            between = lines[lobby + 1:j]
            self.assertIn("wait found >= 1 7200", between, f.name)
            self.assertNotIn("idle 400", between, "%s: a fixed wait races a host at another frame rate" % f.name)
            self.assertLess(between.index("wait found >= 1 7200"), between.index("press 100"), f.name)

    def test_full_match_scripts_wait_for_the_real_end_of_the_match(self):
        # the in-match blocks are counted in frames: a console at 60 runs out of them in half the time and used to leave the match early (lan-plan, 23 Nov)
        for name in ("lan_host_full.txt", "lan_join_full.txt"):
            lines = [ln.strip() for ln in (LAN / name).read_text().splitlines()]
            k = lines.index("log STATE match over")
            self.assertEqual(lines[k - 1], "wait gs == 3 || ingame == 0 || scene == 10 || ovl == 10 54000", name)
            self.assertGreater(k, max(i for i, ln in enumerate(lines) if ln == "endif" and i < k), name)
            self.assertEqual(lines[k + 3], "mash 100 90 ingame == 0 || scene == 10 || ovl == 10 7200", name)   # out of the match's end screens before the results line

    def test_host_scripts_unchanged_and_found_is_a_harness_variable(self):
        harness = (REPO / "native" / "platform" / "harness" / "src" / "np_harness.cpp").read_text()
        self.assertIn('v == "found"', harness)


class Core3GuardEntry(unittest.TestCase):
    """(mp18 bug) HOST A ROOM / JOIN WITH A CODE runs the room, STUN, TURN and up to 10 minutes of waiting with no session and outside Online Central: the fourth core's
    online guard read only NpLanActive() || NpOnlineFlowActive(), so the threads stayed on the system core beside the network stack. The entry now counts (GuardOnline,
    test_core3.cpp has the logic and its mutant); this checks the wiring on the Vita side and the flag's source."""

    def test_platform_hook_passes_the_entry(self):
        src = (REPO / "native/platform/vita/src/np_vita_platform.cpp").read_text()
        self.assertIn('bool NpLanEntryActive() __attribute__((weak));', src)
        self.assertIn("Core3OnlineStep(NpVitaCore3::GuardOnline(NpLanActive(), NpOnlineFlowActive(), NpLanEntryActive && NpLanEntryActive()))", src)
        self.assertNotIn("Core3OnlineStep(NpLanActive() || NpOnlineFlowActive())", src)

    def test_core3_unit_and_entry_mutant(self):
        import shutil
        import tempfile
        cxx = shutil.which("g++") or shutil.which("clang++")
        if not cxx:
            self.skipTest("no C++ compiler")
        src = REPO / "native/platform/vita/tests/test_core3.cpp"
        with tempfile.TemporaryDirectory() as d:
            for extra, want_ok in ((), True), (("-DNP_CORE3_ENTRY_BREAK",), False):
                exe = Path(d) / "t"
                subprocess.run([cxx, "-std=c++17", "-O0", *extra, "-I", str(REPO / "native/platform/vita/src"), str(src), "-o", str(exe), "-lpthread"], check=True)
                r = subprocess.run([str(exe)], capture_output=True, text=True)
                self.assertEqual(r.returncode == 0 and " 0 failures" in r.stdout, want_ok, r.stdout[-300:])

    def test_entry_flag_is_the_menu_flow_or_the_radio_bring_up(self):
        lan = (REPO / "native/platform/sdk/net/np_lan.cpp").read_text()
        self.assertIn("bool NpLanEntryActive() { return g_entering || g_menuFlow; }", lan)
        self.assertIn("bool NpLanEntryActive();", (REPO / "native/platform/sdk/net/np_lan.h").read_text())


class ServiceUrl(unittest.TestCase):
    def setUp(self):
        self.op = load(REPO / "native" / "tools" / "mp" / "online_pair.py", "online_pair_mp18")

    def test_loopback_http_accepted(self):
        self.assertEqual(self.op.service_url_checked("http://127.0.0.1:8797"), "http://127.0.0.1:8797")
        self.assertEqual(self.op.service_url_checked("http://localhost:8797/"), "http://127.0.0.1:8797")

    def test_anything_that_could_reach_the_live_service_refused(self):
        for bad in ("https://strikers.psvita.dev", "http://strikers.psvita.dev", "http://10.0.0.5:8787", "https://127.0.0.1:8797", "http://127.0.0.1:8797/v1", "http://[::1]:8797",
                    "http://127.0.0.1.evil.example:80", "http://127.0.0.1:8797?x=1"):
            with self.assertRaises(ValueError, msg=bad):
                self.op.service_url_checked(bad)

    def test_reset_hook_refused_with_an_external_service(self):
        r = subprocess.run([sys.executable, str(REPO / "native/tools/mp/online_pair.py"), "--out", "/tmp/mp18-never", "--no-proxy", "--service", "--service-url", "http://127.0.0.1:9",
                            "--service-reset-after", "x@1", "--binary", "/bin/false"], capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("only the fake has /__fake/reset", r.stderr)


class ConsoleKit(unittest.TestCase):
    def setUp(self):
        import gen_online as GO
        import online_check as OC
        self.GO, self.OC = GO, OC

    def test_mp_runs_have_routes_rules_and_env(self):
        ids = [sp["id"] for sp in self.GO.RUN_SPECS if sp["id"].startswith("mp-")]
        self.assertEqual(ids, ["mp-hub", "mp-quick", "mp-host", "mp-host-relay", "mp-lan-wifi", "mp-lan-adhoc"])
        for i in ids:
            self.assertIn(i, self.GO.MP18_RULES)
            self.assertTrue((KIT / "env" / ("%s.txt" % i)).exists(), i)
        relay = (KIT / "env" / "mp-host-relay.txt").read_text()
        self.assertIn("MSC_NET_RELAY_ONLY=1", relay)
        self.assertIn("MSC_INPUT_SCRIPT=ux0:data/mscharged-perf-test/on-hostwait.txt", relay)
        self.assertNotIn("MSC_NET_RELAY_ONLY", (KIT / "env" / "mp-host.txt").read_text())
        # the console's env reader refuses a value with '/' or ':' apart from the script path: none of the mp runs carries one
        for i in ids:
            for ln in (KIT / "env" / ("%s.txt" % i)).read_text().splitlines():
                if ln.startswith("MSC_") and not ln.startswith("MSC_INPUT_SCRIPT="):
                    self.assertNotRegex(ln.split("=", 1)[1], r"[/:]", ln)

    def test_routes_pick_the_right_popup_buttons(self):
        wifi = self.GO.route_lanwait(1)
        adhoc = self.GO.route_lanwait(0)
        self.assertIn("mash 4 15 onctx == 1 300\nidle 10\npress 100\nlog chose same wi-fi", wifi)
        self.assertIn("idle 10\npress 100\nlog chose nearby", adhoc)
        self.assertNotIn("log chose online", wifi)
        host = self.GO.route_hostwait()
        self.assertIn("log chose host", host)
        self.assertIn("idle %d\nlog STATE room waited" % self.GO.HOST_WAIT_FRAMES, host)
        for text in (wifi, adhoc, host):
            self.assertTrue(text.rstrip().endswith("exit"))
            self.assertIn("idle 150\nlog STATE back in the main menu", text)   # 5 s: the guard's 3 s return fits before the exit

    def test_committed_route_files_match_the_generator(self):
        for key in ("onhostwait", "lanwifi", "lanadhoc"):
            name, fn, _, _ = self.GO.ROUTES[key]
            self.assertEqual((KIT / "routes" / name).read_text(), fn(), name)

    def test_online_check_self_test(self):
        self.assertEqual(self.OC.self_test(), 0)


if __name__ == "__main__":
    unittest.main()
