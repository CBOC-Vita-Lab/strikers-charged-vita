"""np_vita_envlock.cpp (the real __env_lock / __env_unlock) against its host test, with the empty-lock control, and the link wiring."""
import pathlib, subprocess, unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent


class EnvLock(unittest.TestCase):
    def test_host_test_and_control(self):
        r = subprocess.run(["bash", str(ROOT / "native/platform/vita/tests/run_envlock_test.sh"), "/tmp/np_envlock_pytest"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_linked_directly_and_guarded(self):
        mk = (ROOT / "native/platform/vita/Makefile.vpk").read_text()
        import re
        self.assertRegex(re.search(r"PLATSRC := ((?:[^\n]*\\\n)*[^\n]*)", mk).group(1), r"\bnp_vita_envlock\b")   # a directly linked object: the libc member is never pulled
        self.assertIn("ENV LOCK GUARD", mk)


if __name__ == "__main__":
    unittest.main()
