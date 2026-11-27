"""Mutation check: each mutant is strikers_relay.py with one safety rule broken; test_relay.py must FAIL against it.
    python3 -m unittest test_mutants"""
import os, subprocess, sys, tempfile, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = (HERE / "strikers_relay.py").read_text()

MUTANTS = {
    "path check removed (read allowed anywhere)": ('    raise Refused(f"path not allowed ({mode}): {path!r}: only {DATA}/ and {APP}/ of {TITLE}")', '    return p'),
    "destroy allowed in vc": ('VC_VERBS = ("press",', 'VC_VERBS = ("destroy", "reboot", "press",'),
    "command whitelist open": ('    if not FULL and line not in ("version", "help", f"quit {TITLE}", f"launch {TITLE}"):', '    if False:'),
    "full access on by default": ('FULL = os.environ.get("STRIKERS_RELAY_FULL", "0") == "1" or', 'FULL = True or'),
    "restore skipped": ('def do_restore():\n', 'def do_restore():\n    return "skipped"\n'),
    "restore sha check removed": ('        if sha(rget(NP)) != sha(keep):', '        if False:'),
    "failed backup read does not stop": ('    except ftplib.error_perm:\n        previous = None\n        log("put eboot: no eboot', '    except Exception:\n        previous = None\n        log("put eboot: no eboot'),
    "eboot readback not verified": ('            if rget(EBOOT) == data:', '            if True:'),
    "eboot backed up every time": ('if previous is not None and BACKUP_NAME not in names(APP):', 'if previous is not None:'),
    "galaxy check ignored": ('def need_free_vita():\n', 'def need_free_vita():\n    return\n'),
    "delete anything": ('    elif mode == "delete":', '    elif mode == "delete" and (p.startswith("ux0:") or True):\n        return p\n    elif mode == "delete":'),
    "owner file not kept": ('def owner_backup():\n', 'def owner_backup():\n    return\n'),
    "batch always-steps skipped": ('if failed and not st.get("always"):', 'if failed:'),
    "game not closed before pull": ('    if req.get("close", True):', '    if False:'),
    "eswap pushes again every time": ('and st.read_text().strip() == name and not req.get("force"):', 'and False:'),
    "cache gate open": ('    elif mode == "cachelist":', '    elif mode == "cachelist" and p.startswith("ux0:"):\n        return p\n    elif mode == "cachelist":'),
    "cache pull leaves the game running": ('    if req.get("close") is not False:', '    if False:'),
    "old log not deleted": ('    step("old log out", clear_log)', '    pass'),
}


def run_tests(src_text):
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "strikers_relay_mutant.py"
        p.write_text(src_text)
        env = dict(os.environ, STRIKERS_RELAY_SRC=str(p))
        r = subprocess.run([sys.executable, "-m", "unittest", "-f", "test_relay"], cwd=str(HERE), env=env, capture_output=True, text=True)
        return r.returncode


class Mutants(unittest.TestCase):
    def test_each_mutant_is_caught(self):
        survivors = []
        for name, (old, new) in MUTANTS.items():
            self.assertEqual(SRC.count(old), 1, f"mutation anchor for {name!r} not found exactly once")
            if run_tests(SRC.replace(old, new)) == 0:
                survivors.append(name)
        self.assertEqual(survivors, [], f"mutants that the tests did not catch: {survivors}")


if __name__ == "__main__":
    unittest.main()
