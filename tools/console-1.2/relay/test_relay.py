"""End-to-end tests of strikers_relay.py against fake_vita.py (nothing leaves this machine).
    python3 -m unittest test_relay        (or pytest)
STRIKERS_RELAY_SRC=<file> runs the same tests against another copy of the relay (test_mutants.py does that)."""
import importlib.util, json, os, re, sys, tempfile, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from fake_vita import FakeVita

_tmp_root = tempfile.mkdtemp(prefix="strikers-relay-test-")
os.environ["STRIKERS_RELAY_ROOT"] = _tmp_root
spec = importlib.util.spec_from_file_location("strikers_relay_under_test", os.environ.get("STRIKERS_RELAY_SRC") or HERE / "strikers_relay.py")
R = importlib.util.module_from_spec(spec); spec.loader.exec_module(R)
R.sleep = lambda s: __import__("time").sleep(min(s, 0.05))
R.RETRY_S = 0

D = "ux0:data/mscharged-perf-test"
A = "ux0:app/CDXMP0001"
OWNER_ENV = b"# the owner's own settings\nMSC_X=1\n"
ENV = b"MSC_INPUT_SCRIPT=ux0:data/mscharged-perf-test/tour.txt\nMSC_FRAME_TIMING=300\n"
RUNQ = dict(est_s=0, first_poll_s=0, poll_s=0.05, nap_s=0, stall_s=0.3, launch_wait_s=1, launch_poll_s=0.05, relaunch_nap_s=0, cap_s=1.5)


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="vita-"))
        self.v = FakeVita(self.tmp / "console")
        R.HOST, R.FTP_PORT, R.CMD_PORT = "127.0.0.1", self.v.ftp_port, self.v.cmd_port
        base = Path(tempfile.mkdtemp(prefix="relay-"))
        R.ROOT = base; R.INBOX, R.OUTBOX, R.STAGING, R.DONE, R.STATE = (base / n for n in ("inbox", "outbox", "staging", "done", "state"))
        R.LOG = base / "relay.log"; R.setup_dirs(); R.EBOOT_OK = True
        R.galaxy_running = lambda: False
        self.n = 0
        self.v.put_file(f"{D}/np-player.txt", OWNER_ENV)
        self.v.put_file(f"{A}/eboot.bin", b"OLD-EBOOT")
        self.v.put_file("ux0:data/MSCN00003-save/x", b"other app data")
        self.v.put_file("ux0:tai/config.txt", b"plugins")
        self.v.put_file("ux0:app/MSCN00003/eboot.bin", b"installed port")

    def tearDown(self):
        self.v.close()

    def stage(self, name, data): (R.STAGING / name).write_bytes(data)

    def job(self, req):
        self.n += 1
        f = R.INBOX / f"{5000 + self.n}-t.json"; f.write_text(json.dumps(req))
        res = R.process(f)
        res["out"] = R.OUTBOX / f.stem
        return res

    def assertUntouched(self, *needles):
        log = " | ".join(self.v.ftp_log) + " | " + " | ".join(self.v.cmd_log)
        for n in needles: self.assertNotIn(n, log)


class Actions(Base):
    def test_status_ls_get(self):
        r = self.job({"action": "status"}); self.assertTrue(r["ok"], r)
        self.assertIn("fake vitacompanion", r["result"]["version"])
        r = self.job({"action": "ls", "path": D}); self.assertTrue(r["ok"], r)
        self.assertIn("np-player.txt", (r["out"] / "listing.txt").read_text())
        r = self.job({"action": "ls", "path": A}); self.assertTrue(r["ok"], r)
        r = self.job({"action": "get", "path": f"{D}/np-player.txt"}); self.assertTrue(r["ok"], r)
        self.assertEqual((r["out"] / "np-player.txt").read_bytes(), OWNER_ENV)
        r = self.job({"action": "get", "path": f"{A}/eboot.bin"}); self.assertTrue(r["ok"], r)

    def test_put_env_route_keeps_owner_file(self):
        self.stage("env-a.txt", ENV); self.stage("tour.txt", b"route\n")
        r = self.job({"action": "put", "src": "env-a.txt", "path": f"{D}/np-player.txt"}); self.assertTrue(r["ok"], r)
        self.assertEqual(self.v.read_file(f"{D}/np-player.txt"), ENV)
        self.assertEqual((R.STATE / "owner-np-player.txt").read_bytes(), OWNER_ENV)
        r = self.job({"action": "put", "src": "tour.txt", "path": f"{D}/tour.txt"}); self.assertTrue(r["ok"], r)
        self.assertEqual(self.v.read_file(f"{D}/tour.txt"), b"route\n")
        # the owner's file is kept once: a second put does not overwrite the kept copy with the session's
        self.job({"action": "put", "src": "env-a.txt", "path": f"{D}/np-player.txt"})
        self.assertEqual((R.STATE / "owner-np-player.txt").read_bytes(), OWNER_ENV)

    def test_restore_present_and_sha_check(self):
        self.stage("env-a.txt", ENV)
        self.job({"action": "put", "src": "env-a.txt", "path": f"{D}/np-player.txt"})
        r = self.job({"action": "restore"}); self.assertTrue(r["ok"], r)
        self.assertEqual(self.v.read_file(f"{D}/np-player.txt"), OWNER_ENV)

    def test_restore_none_deletes_sessions_file(self):
        (self.v.local(f"{D}/np-player.txt")).unlink()
        self.stage("env-a.txt", ENV)
        self.job({"action": "put", "src": "env-a.txt", "path": f"{D}/np-player.txt"})
        r = self.job({"action": "restore"}); self.assertTrue(r["ok"], r)
        self.assertIsNone(self.v.read_file(f"{D}/np-player.txt"))

    def test_restore_fails_when_readback_differs(self):
        self.stage("env-a.txt", ENV)
        self.job({"action": "put", "src": "env-a.txt", "path": f"{D}/np-player.txt"})
        (R.STATE / "owner-np-player.txt").write_bytes(OWNER_ENV)
        orig = R.rget
        R.rget = lambda p: b"something else" if p == R.NP else orig(p)
        try:
            r = self.job({"action": "restore"})
        finally:
            R.rget = orig
        self.assertFalse(r["ok"]); self.assertIn("differs", r["error"])

    def test_launch_close_vc(self):
        self.assertTrue(self.job({"action": "launch"})["ok"])
        self.assertIn("launch CDXMP0001", self.v.cmd_log)
        self.assertTrue(self.job({"action": "close"})["ok"])
        self.assertIn("quit CDXMP0001", self.v.cmd_log)
        for c in ("press cross", "release cross; wait 1", "screen on", "unlock", "nosleep", "capture 0"):
            self.assertTrue(self.job({"action": "vc", "cmd": c, "wait": 0.1})["ok"], c)

    def test_pull_logs_lean_and_closes_first(self):
        for n in ("log.txt", "stderr.txt", "env.txt", "env.txt.old1", "log.txt.prev", "settings.txt", "crash.txt", "other.txt"):
            self.v.put_file(f"{D}/{n}", n.encode())
        r = self.job({"action": "pull_logs", "nap_s": 0}); self.assertTrue(r["ok"], r)
        got = sorted(p.name for p in r["out"].iterdir() if p.name != "result.json")
        self.assertEqual(got, ["crash.txt", "env.txt", "log.txt", "np-player.txt", "settings.txt", "stderr.txt"])
        self.assertEqual(self.v.cmd_log[0], "quit CDXMP0001")
        r = self.job({"action": "pull_logs", "all": True, "close": False}); self.assertTrue(r["ok"], r)
        self.assertTrue((r["out"] / "env.txt.old1").exists() and (r["out"] / "log.txt.prev").exists())
        self.assertEqual(self.v.cmd_log.count("quit CDXMP0001"), 1)

    def test_pull_cache_reads_only_the_shader_cache(self):
        C = f"{D}/shadercache"
        files = {f"{C}/vgx_shaderlist.bin": b"LIST", f"{C}/vgx_shaderlist.stamp": b"S", f"{C}/874a13e0/v3/f/1D/1D26B4DDD84890A8.gxp": b"FRAG",
                 f"{C}/874a13e0/v3/v/EB/EB22BD45FC64D939.gxp": b"VERT", f"{C}/874a13e0/cache-moved": b"x", f"{C}/874a13e0/v3/f/1D/notes.txt": b"no"}
        for k, v in files.items(): self.v.put_file(k, v)
        r = self.job({"action": "pull_cache", "nap_s": 0}); self.assertTrue(r["ok"], r)
        o = r["out"] / "shadercache"
        self.assertEqual((o / "vgx_shaderlist.bin").read_bytes(), b"LIST")
        self.assertEqual((o / "874a13e0/v3/f/1D/1D26B4DDD84890A8.gxp").read_bytes(), b"FRAG")
        self.assertEqual((o / "874a13e0/v3/v/EB/EB22BD45FC64D939.gxp").read_bytes(), b"VERT")
        self.assertFalse((o / "874a13e0/v3/f/1D/notes.txt").exists())   # only .gxp files
        self.assertEqual(self.v.cmd_log[0], "quit CDXMP0001")
        self.assertUntouched("STOR", "DELE", "MKD", "MSCN00003", "tai")   # read-only
        r2 = self.job({"action": "pull_cache", "close": False}); self.assertTrue(r2["ok"], r2)
        self.assertTrue((r2["out"] / "shadercache/vgx_shaderlist.bin").exists())
        # the cache listing gate: nothing outside shadercache/<key>/v3/{v,f}/XX
        for bad in (f"{D}", f"{D}/shots", f"{D}/shadercache/../x", f"{D}/shadercache/874a13e0/v2", f"{D}/shadercache/874a13e0/v3/x", f"{D}/shadercache/a/v3/v/EB/x/y", A, "ux0:tai"):
            with self.assertRaises(R.Refused): R.check_path(bad, "cachelist")

    def test_shot_marked(self):
        self.v.put_file("ux0:data/vitacompanion/plane0.bmp", b"BM....")
        r = self.job({"action": "shot", "name": "s1"}); self.assertTrue(r["ok"], r)
        self.assertIn("NOT FOR JUDGING PICTURES", r["result"])

    def test_batch_sleep_and_always(self):
        self.stage("env-a.txt", ENV)
        r = self.job({"action": "batch", "steps": [
            {"action": "put", "src": "env-a.txt", "path": f"{D}/np-player.txt"},
            {"sleep": 0.01},
            {"action": "get", "path": "ux0:tai/config.txt"},       # refused: the batch fails here
            {"action": "status"},                                  # skipped
            {"action": "restore", "always": True}]})
        self.assertFalse(r["ok"])
        res = json.loads(r["error"].split(": ", 1)[1])
        self.assertIn("SKIPPED", res[3]); self.assertNotIn("error", res[4])
        self.assertEqual(self.v.read_file(f"{D}/np-player.txt"), OWNER_ENV)   # the restore at the end ran

    def test_sleep_action(self):
        self.assertTrue(self.job({"action": "sleep", "s": 0.01})["ok"])

    def test_retry_on_426(self):
        self.stage("env-a.txt", ENV)
        self.v.fail[("STOR", "np-player.txt")] = 2
        r = self.job({"action": "put", "src": "env-a.txt", "path": f"{D}/np-player.txt"}); self.assertTrue(r["ok"], r)
        self.assertEqual(self.v.read_file(f"{D}/np-player.txt"), ENV)


class Eboot(Base):
    def test_backup_once_and_verify(self):
        self.stage("e1", b"NEW-1" * 1000); self.stage("e2", b"NEW-2" * 1000)
        r = self.job({"action": "put", "src": "e1", "path": f"{A}/eboot.bin"}); self.assertTrue(r["ok"], r)
        self.assertEqual(self.v.read_file(f"{A}/eboot.bin"), b"NEW-1" * 1000)
        self.assertEqual(self.v.read_file(f"{A}/eboot.before-claude-relay.bin"), b"OLD-EBOOT")
        r = self.job({"action": "put", "src": "e2", "path": f"{A}/eboot.bin"}); self.assertTrue(r["ok"], r)
        self.assertEqual(self.v.read_file(f"{A}/eboot.before-claude-relay.bin"), b"OLD-EBOOT")   # once only
        self.assertEqual(self.v.read_file(f"{A}/eboot.bin"), b"NEW-2" * 1000)

    def test_failed_backup_read_leaves_eboot_alone(self):
        self.stage("e1", b"NEW-1")
        self.v.fail[("RETR", "eboot.bin")] = 99
        r = self.job({"action": "put", "src": "e1", "path": f"{A}/eboot.bin"})
        self.assertFalse(r["ok"])
        self.assertEqual(self.v.read_file(f"{A}/eboot.bin"), b"OLD-EBOOT")
        self.assertNotIn("STOR ux0:app/CDXMP0001/eboot.bin", self.v.ftp_log)
        self.assertIsNone(self.v.read_file(f"{A}/eboot.before-claude-relay.bin"))

    def test_backup_mismatch_stops(self):
        self.stage("e1", b"NEW-1")
        orig = R.rget
        n = {"c": 0}
        def bad(p):
            if p.endswith(R.BACKUP_NAME): return b"torn"
            return orig(p)
        R.rget = bad
        try: r = self.job({"action": "put", "src": "e1", "path": f"{A}/eboot.bin"})
        finally: R.rget = orig
        self.assertFalse(r["ok"]); self.assertEqual(self.v.read_file(f"{A}/eboot.bin"), b"OLD-EBOOT")

    def test_stor_retry_and_final_failure_puts_previous_back(self):
        self.stage("e1", b"NEW-1")
        self.v.fail[("STOR", "eboot.bin")] = 1
        self.assertTrue(self.job({"action": "put", "src": "e1", "path": f"{A}/eboot.bin"})["ok"])
        self.assertEqual(self.v.read_file(f"{A}/eboot.bin"), b"NEW-1")

    def test_eboot_readback_mismatch_detected(self):
        self.stage("e1", b"NEW-1")
        orig = R.rget
        R.rget = lambda p: b"torn" if p == R.EBOOT and self.v.read_file(f"{A}/{R.BACKUP_NAME}") is not None else orig(p)
        try: r = self.job({"action": "put", "src": "e1", "path": f"{A}/eboot.bin"})
        finally: R.rget = orig
        self.assertFalse(r["ok"]); self.assertIn("failed after", r["error"])

    def test_no_launch_after_failed_install(self):
        R.EBOOT_OK = False
        r = self.job({"action": "launch"}); self.assertFalse(r["ok"])
        self.assertNotIn("launch CDXMP0001", self.v.cmd_log)


class Swap(Base):
    def test_eswap_plain_pgo_and_back(self):
        self.stage("eb-plain", b"PLAIN" * 100); self.stage("eb-pgo", b"PGO" * 100)
        for name, src, want in (("r17", "eb-plain", b"PLAIN" * 100), ("r17pgox", "eb-pgo", b"PGO" * 100), ("r17", "eb-plain", b"PLAIN" * 100)):
            r = self.job({"action": "eswap", "name": name, "src": src, "nap_s": 0}); self.assertTrue(r["ok"], r)
            self.assertEqual(self.v.read_file(f"{A}/eboot.bin"), want)
        self.assertEqual((R.STATE / "eboot-name").read_text().strip(), "r17")
        self.assertEqual(self.v.read_file(f"{A}/eboot.before-claude-relay.bin"), b"OLD-EBOOT")
        self.assertIn("quit CDXMP0001", self.v.cmd_log)

    def test_eswap_skips_when_already_there_unless_forced(self):
        self.stage("eb", b"X" * 10)
        self.job({"action": "eswap", "name": "r17", "src": "eb", "nap_s": 0})
        n = len([l for l in self.v.ftp_log if l.startswith("STOR")])
        r = self.job({"action": "eswap", "name": "r17", "src": "eb", "nap_s": 0}); self.assertIn("already", r["result"])
        self.assertEqual(n, len([l for l in self.v.ftp_log if l.startswith("STOR")]))
        self.job({"action": "eswap", "name": "r17", "src": "eb", "nap_s": 0, "force": True})
        self.assertGreater(len([l for l in self.v.ftp_log if l.startswith("STOR")]), n)

    def test_eswap_failure_marks_unknown(self):
        self.stage("eb", b"X" * 10)
        self.v.fail[("RETR", "eboot.bin")] = 99
        self.assertFalse(self.job({"action": "eswap", "name": "r17", "src": "eb", "nap_s": 0})["ok"])
        self.assertEqual((R.STATE / "eboot-name").read_text().strip(), "unknown")

    def test_eswap_bad_name(self):
        self.stage("eb", b"X")
        self.assertFalse(self.job({"action": "eswap", "name": "../x", "src": "eb"})["ok"])

    def test_hold_files_are_not_taken(self):
        (R.INBOX / "5009-x.json.hold").write_text('{"action":"launch"}')
        self.assertEqual(list(R.INBOX.glob("*.json")), [])


class Run(Base):
    def run_job(self, **kw):
        self.stage("env-r.txt", ENV); self.stage("tour.txt", b"route\n")
        req = dict(action="run", id="d1", env="env-r.txt", route="tour.txt", route_name="tour.txt", **RUNQ); req.update(kw)
        return self.job(req)

    def test_run_done(self):
        r = self.run_job(); self.assertTrue(r["ok"], r)
        res = r["result"]
        self.assertEqual(res["status"], "done")
        log = (r["out"] / "log.txt").read_text()
        self.assertTrue(log.startswith("===== fake build"))   # sliced from the last build header
        self.assertIn("script exit", log)
        self.assertEqual(self.v.read_file(f"{D}/tour.txt"), b"route\n")
        self.assertEqual(self.v.read_file(f"{D}/np-player.txt"), ENV)
        self.assertEqual((r["out"] / "status.txt").read_text().strip(), "done")
        self.assertTrue(all("[" in s and " s]" in s for s in res["steps"]))
        self.assertGreaterEqual(len(res["steps"]), 8)
        # the game was closed before the log was pulled
        self.assertEqual(self.v.cmd_log[0:2], ["quit CDXMP0001", "launch CDXMP0001"])
        self.assertEqual(self.v.cmd_log[-1], "quit CDXMP0001")

    def test_run_deletes_old_log(self):
        self.v.put_file(f"{D}/log.txt", b"===== old build\n[script] script exit\n")
        r = self.run_job(); self.assertEqual(r["result"]["status"], "done")
        self.assertNotIn("old build", (r["out"] / "log.txt").read_text())

    def test_run_crash_is_stalled(self):
        self.v.launch_mode = "crash"
        r = self.run_job(); self.assertTrue(r["ok"], r); self.assertEqual(r["result"]["status"], "stalled")
        self.assertEqual(self.v.cmd_log[-1], "quit CDXMP0001")

    def test_run_nolaunch(self):
        self.v.launch_mode = "nolaunch"
        r = self.run_job(); self.assertEqual(r["result"]["status"], "nolaunch")
        self.assertEqual(self.v.cmd_log.count("launch CDXMP0001"), 2)

    def test_run_time_cap(self):
        self.v.launch_mode = "crash"
        r = self.run_job(stall_s=999, cap_s=0.4); self.assertEqual(r["result"]["status"], "timeout")

    def test_run_shots_and_settings_hook(self):
        self.v.put_file(f"{D}/shots/old.png", b"x")
        self.v.put_file(f"{D}/shots/new0.png", b"y")
        r = self.run_job(shots=True, settings="a b")
        self.assertTrue(r["ok"], r)
        self.assertIsNone(self.v.read_file(f"{D}/np-settings-open"))
        self.assertTrue(any("DELE ux0:data/mscharged-perf-test/shots/old.png" == l for l in self.v.ftp_log))

    def test_run_then_restore_gives_owner_file_back(self):
        self.run_job()
        self.job({"action": "restore"})
        self.assertEqual(self.v.read_file(f"{D}/np-player.txt"), OWNER_ENV)

    def test_run_refuses_bad_route_name_and_missing_staging(self):
        self.assertFalse(self.run_job(route_name="../tai.txt")["ok"])
        self.assertFalse(self.job(dict(action="run", id="x", env="nope.txt", **RUNQ))["ok"])
        self.assertUntouched("tai")

    def test_run_crash_dump_listed(self):
        self.v.launch_mode = "normal"
        orig = R.dump_names
        calls = {"n": 0}
        def dn():
            calls["n"] += 1
            return [] if calls["n"] == 1 else ["psp2core-1-x.psp2dmp"]
        R.dump_names = dn
        try: r = self.run_job()
        finally: R.dump_names = orig
        self.assertEqual((r["out"] / "dumps.txt").read_text().strip(), "psp2core-1-x.psp2dmp")


class HardLimits(Base):
    FORBIDDEN_READS = ["ux0:app/MSCN00003/eboot.bin", "ux0:data/MSCN00003-save/x", "ux0:tai/config.txt", "ur0:tai/config.txt",
                       "ux0:data/mscharged-perf-test/../MSCN00003/x", "ux0:data/mscharged/game/x", "ux0:data/vitacompanion/plane0.bmp",
                       "ux0:app/CDXMP0001/../MSCN00003/eboot.bin", "ux0:data", "ux0:data/other.txt", "ux0:app/SMGV00001/eboot.bin"]

    def test_get_refused_everywhere_else(self):
        for p in self.FORBIDDEN_READS:
            r = self.job({"action": "get", "path": p}); self.assertFalse(r["ok"], p); self.assertIn("not allowed", r["error"])
        self.assertEqual(self.v.touched(), [])

    def test_ls_refused(self):
        for p in ("ux0:app", "ux0:app/MSCN00003", "ux0:tai", "ur0:tai", "ux0:data", "ux0:data/mscharged", f"{D}/../x"):
            self.assertFalse(self.job({"action": "ls", "path": p})["ok"], p)
        self.assertEqual(self.v.touched(), [])

    def test_put_refused_everywhere_else(self):
        self.stage("p.txt", b"x"); self.stage("p.skprx", b"x")
        for p in ("ux0:app/MSCN00003/eboot.bin", "ux0:tai/config.txt", "ur0:tai/config.txt", "ux0:data/MSCN00003/x.txt", f"{D}/sub/x.txt",
                  f"{D}/x.skprx", f"{D}/../x.txt", f"{A}/sce_sys/param.sfo", f"{A}/shadercache/x", f"{A}/{R.BACKUP_NAME}", "ux0:data/x.txt", "ux0:app/CDXMP0001x/eboot.bin"):
            r = self.job({"action": "put", "src": "p.txt", "path": p}); self.assertFalse(r["ok"], p)
        self.assertEqual([l for l in self.v.ftp_log if l.startswith("STOR")], [])

    def test_delete_gate(self):
        for p in ("ux0:app/MSCN00003/eboot.bin", "ux0:tai/config.txt", f"{A}/eboot.bin", f"{D}/tour.txt", f"{D}/shots/../log.txt", "ux0:data/MSCN00003-save/x"):
            with self.assertRaises(R.Refused): R.rdel(p)
        self.assertEqual([l for l in self.v.ftp_log if l.startswith("DELE")], [])
        R.rdel(f"{D}/log.txt")   # allowed ones

    def test_destroy_reboot_and_other_launch_refused(self):
        for c in ("destroy", "reboot", "launch MSCN00003", "launch CDXMP0001", "quit all", "destroy; press cross", "press cross; reboot", "vitashell", "", "quit CDXMP0001"):
            r = self.job({"action": "vc", "cmd": c}); self.assertFalse(r["ok"], c)
        self.assertEqual(self.v.cmd_log, [])
        for c in ("destroy", "reboot", "launch MSCN00003", "quit all", "quit MSCN00003", "pkg install x"):
            with self.assertRaises(R.Refused): R.command(c)
        self.assertEqual(self.v.cmd_log, [])

    def test_no_close_action_reaches_other_titles(self):
        self.job({"action": "close"}); self.assertEqual(self.v.cmd_log, ["quit CDXMP0001"])

    def test_unknown_action(self):
        for a in ("destroy", "reboot", "install", "delete", "rm", "clear_input", "tidy"):
            self.assertFalse(self.job({"action": a})["ok"], a)
        self.assertEqual(self.v.cmd_log, [])

    def test_galaxy_relay_running_refuses_jobs(self):
        R.galaxy_running = lambda: True
        for a in ({"action": "launch"}, {"action": "ls"}, {"action": "run", "id": "x", "env": "e"}, {"action": "vc", "cmd": "wait 1"}):
            r = self.job(a); self.assertFalse(r["ok"]); self.assertIn("Galaxy", r["error"])
        self.assertEqual(self.v.cmd_log, []); self.assertEqual(self.v.ftp_log, [])

    def test_galaxy_appearing_mid_batch_stops(self):
        calls = {"n": 0}
        def g():
            calls["n"] += 1; return calls["n"] > 1
        R.galaxy_running = g
        r = self.job({"action": "batch", "steps": [{"action": "ls"}, {"action": "ls"}]})
        self.assertFalse(r["ok"]); self.assertIn("Galaxy", r["error"])

    def test_main_refuses_to_start_with_galaxy(self):
        R.galaxy_running = lambda: True
        os.environ["VITA"] = "127.0.0.1"
        try:
            with self.assertRaises(SystemExit) as c: R.main()
        finally: del os.environ["VITA"]
        self.assertIn("Galaxy", str(c.exception))

    def test_main_needs_an_address(self):
        old = R.vita_address
        R.vita_address = lambda: ""
        try:
            with self.assertRaises(SystemExit): R.main()
        finally: R.vita_address = old

    def test_address_from_env_or_ignored_file(self):
        os.environ["VITA"] = "10.0.0.9"
        try: self.assertEqual(R.vita_address(), "10.0.0.9")
        finally: del os.environ["VITA"]
        gi = (HERE / ".gitignore").read_text()
        self.assertIn("vita-address.txt", gi)

    def test_no_address_in_the_repository_files(self):
        for p in HERE.iterdir():
            if p.name == "vita-address.txt":
                continue  # the git-ignored local address file is meant to hold the address
            if p.suffix in (".py", ".md", ".command", ".txt") or p.name == ".gitignore":
                for m in re.finditer(r"\b(\d{1,3}\.){3}\d{1,3}\b", p.read_text()):
                    self.assertIn(m.group(0), ("127.0.0.1", "10.0.0.9", "0.0.0.0"), f"{p.name}: {m.group(0)}")

    def test_dump_names_only_psp2core(self):
        self.v.put_file("ux0:data/psp2core-1-a.psp2dmp", b"d"); self.v.put_file("ux0:data/secret.txt", b"s")
        self.assertEqual(R.dump_names(), ["psp2core-1-a.psp2dmp"])
        r = self.job({"action": "get", "path": "ux0:data/psp2core-1-a.psp2dmp"}); self.assertTrue(r["ok"], r)


class FullAccess(Base):
    def setUp(self):
        super().setUp(); R.FULL = True
    def tearDown(self):
        R.FULL = False; super().tearDown()

    def test_profile_copy_with_backup(self):
        prof = b"PSVS0300" + bytes([11,0,0,0, 0xf4,1,0,0, 0xde,0,0,0, 0xde,0,0,0, 0xa6,0,0,0])
        self.v.put_file("ur0:data/PSVshell/profiles/SMGV00001", prof)
        self.v.put_file("ur0:data/PSVshell/profiles/CDXMP0001", b"OLD")
        r = self.job({"action": "get", "path": "ur0:data/PSVshell/profiles/SMGV00001"}); self.assertTrue(r["ok"], r)
        got = (r["out"] / "SMGV00001").read_bytes(); self.assertEqual(got, prof)
        self.stage("CDXMP0001", got)
        r = self.job({"action": "put", "src": "CDXMP0001", "path": "ur0:data/PSVshell/profiles/CDXMP0001"}); self.assertTrue(r["ok"], r)
        self.assertEqual(self.v.read_file("ur0:data/PSVshell/profiles/CDXMP0001"), prof)
        kept = list((R.ROOT / "backups").rglob("CDXMP0001"))
        self.assertEqual(len(kept), 1); self.assertEqual(kept[0].read_bytes(), b"OLD")

    def test_rm_keeps_a_copy_and_dotdot_still_refused(self):
        self.v.put_file("ux0:data/x/y.txt", b"Y")
        r = self.job({"action": "rm", "path": "ux0:data/x/y.txt"}); self.assertTrue(r["ok"], r)
        self.assertEqual([k.read_bytes() for k in (R.ROOT / "backups").rglob("y.txt")], [b"Y"])
        self.assertFalse(self.job({"action": "get", "path": "ux0:data/x/../y"})["ok"])

    def test_full_off_refuses_new_actions(self):
        R.FULL = False
        for a in ({"action": "rm", "path": "ux0:data/x"}, {"action": "cmd", "line": "reboot"}):
            self.assertFalse(self.job(a)["ok"], a)


if __name__ == "__main__":
    unittest.main()
