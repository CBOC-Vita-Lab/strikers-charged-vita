"""mkjobs.py against the kit's real files, then the jobs it writes carried out by the relay on the fake Vita, then `results` and `ab`."""
import json, subprocess, sys, tempfile, unittest
from pathlib import Path
import test_relay as T

HERE = Path(__file__).resolve().parent


def mk(*args):
    return subprocess.run([sys.executable, str(HERE / "mkjobs.py"), *args], capture_output=True, text=True)


class MkJobs(T.Base):
    def setUp(self):
        super().setUp()
        self.rel = Path(tempfile.mkdtemp(prefix="mk-")); self.ver = Path(tempfile.mkdtemp(prefix="ver-"))
        for n, b in (("r17", b"PLAIN"), ("r17pgox", b"PGO")):
            (self.ver / n).mkdir(); (self.ver / n / "eboot.bin").write_bytes(b)

    def jobs(self, *a):
        r = mk("jobs", "r17-sitting.txt", "--relay", str(self.rel), "--versions", str(self.ver), *a); self.assertEqual(r.returncode, 0, r.stderr + r.stdout); return r

    def test_jobs_written_numbered_with_restore_last(self):
        self.jobs("--ids", "d1,lock60tour", "--end-with", "r17")
        names = sorted(p.name for p in (self.rel / "inbox").glob("*.json"))
        self.assertEqual(names, ["5000-d1.json", "5001-lock60tour.json", "5002-eswap-r17.json", "5003-restore.json"])
        d1 = json.loads((self.rel / "inbox" / names[0]).read_text())
        self.assertEqual([s["action"] for s in d1["steps"]], ["put", "run"])      # eboot of the list's eswap r17, then the run
        self.assertEqual(d1["steps"][1]["route_name"], "fb-tour.txt")
        self.assertEqual(json.loads((self.rel / "inbox" / names[1]).read_text())["steps"][0]["action"], "run")   # eboot only once
        self.assertEqual(json.loads((self.rel / "inbox" / names[3]).read_text())["steps"], [{"action": "restore", "always": True}])
        self.assertTrue((self.rel / "staging" / "env-d1.txt").is_file() and (self.rel / "staging" / "eboot-r17.bin").is_file())
        self.jobs("--ids", "worstgpu1")   # numbering goes on after the highest one
        self.assertTrue((self.rel / "inbox" / "5004-worstgpu1.json").exists() or any(p.name.startswith("5004-") for p in (self.rel / "inbox").glob("*")))

    def test_unit_on_online_runs_queue(self):
        """online24: the ON unit of the extra list queues its six runs (one console, the real service), the settings one with its key words, and the owner's np-player.txt comes back last"""
        r = mk("jobs", str(HERE.parent / "r17-sitting-extra.txt"), "--unit", "ON", "--relay", str(self.rel), "--versions", str(self.ver), "--assume-eboot", "r17")
        self.assertEqual(r.returncode, 0, r.stderr + r.stdout)
        jobs = [json.loads(p.read_text()) for p in sorted((self.rel / "inbox").glob("*.json"))]
        runs = [s for j in jobs for s in j["steps"] if s["action"] == "run"]
        self.assertEqual([s["id"] for s in runs], ["on-hub", "on-hub-old", "on-lists", "on-quick", "on-ranked", "on-report"])
        self.assertEqual([s["route_name"] for s in runs[:5]], ["on-hub.txt", "on-hub.txt", "on-lists.txt", "on-quick.txt", "on-ranked.txt"])
        self.assertIn("rtrigger", runs[5]["settings"])
        self.assertEqual(jobs[-1]["steps"], [{"action": "restore", "always": True}])
        env = (self.rel / "staging" / "env-on-hub-old.txt").read_text()
        self.assertIn("MSC_ONLINE_HOST=old", env)
        self.assertNotIn("MSC_ONLINE_URL", env)

    def test_unit_hold_release_and_errors(self):
        self.jobs("--unit", "A", "--hold", "--assume-eboot", "r17")
        self.assertEqual(list((self.rel / "inbox").glob("*.json")), [])
        self.assertGreater(len(list((self.rel / "inbox").glob("*.json.hold"))), 5)
        self.assertEqual(mk("release", "5000", "--relay", str(self.rel)).returncode, 0)
        self.assertEqual([p.name for p in (self.rel / "inbox").glob("*.json")], ["5000-d1.json"])
        self.assertNotEqual(mk("jobs", "r17-sitting.txt", "--ids", "nosuchrun", "--relay", str(self.rel), "--dry").returncode, 0)

    def test_end_to_end_results_and_ab(self):
        self.jobs("--ids", "d1,worstgpu1", "--end-with", "r17")
        for p in sorted((self.rel / "inbox").glob("*.json")):
            req = json.loads(p.read_text())
            for s in req.get("steps", []):
                if s["action"] == "run": s.update(T.RUNQ)
                if s["action"] == "put": s["path"] = s["path"]
            if req.get("action") == "eswap": req["nap_s"] = 0
            for s in req.get("steps", []):
                s.setdefault("nap_s", 0)
            self.stage_from(self.rel / "staging")
            res = self.job(req); self.assertTrue(res["ok"], res.get("error"))
            if req.get("action") == "batch" and any(s["action"] == "run" for s in req["steps"]):
                # keep the outbox folder under the mkjobs name so `results` finds it
                import shutil, test_relay
                shutil.copytree(res["out"], self.rel / "outbox" / p.stem, dirs_exist_ok=True)
        self.assertEqual(self.v.read_file(f"{T.D}/np-player.txt"), T.OWNER_ENV)
        r = mk("results", "--relay", str(self.rel)); self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("d1: pass", r.stdout); self.assertIn("worstgpu1: pass", r.stdout)
        r = mk("ab", "d1", "worstgpu1", "--relay", str(self.rel)); self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("== play", r.stdout); self.assertIn("d: fps", r.stdout)

    def stage_from(self, d):
        for p in d.iterdir(): self.stage(p.name, p.read_bytes())

    def test_results_flags_invalid(self):
        out = self.rel / "outbox" / "5000-x"; (out / "00").mkdir(parents=True)
        (out / "result.json").write_text(json.dumps({"ok": True, "result": [{"action": "run", "result": {"id": "d1", "status": "stalled", "route_wait_s": 1, "total_s": 2}}]}))
        (out / "00" / "log.txt").write_text("===== build\nnothing\n")
        r = mk("results", "--relay", str(self.rel)); self.assertIn("d1: INVALID", r.stdout)


if __name__ == "__main__":
    unittest.main()
