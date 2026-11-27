"""Messages off the wire against the game's fixed arrays (online audit, 1.3).

- native/tools/mp/lan_lobby_bounds: the lobby's connection messages with sizes and counts larger than their storage (AddressSanitizer).
- native/tools/mp/tourn_bounds: NetTournManager::HandleTournamentGameUpdate with game indices 7..255 (mGames holds 7).
Both build the game's own source files on the host; both failed before their fixes.
"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _run(script):
    with tempfile.TemporaryDirectory() as out:
        return subprocess.run(["bash", str(ROOT / script), out], capture_output=True, text=True)


@unittest.skipUnless(shutil.which("g++"), "needs g++")
class OnlineBoundsTests(unittest.TestCase):
    def test_lobby_messages(self):
        run = _run("native/tools/mp/lan_lobby_bounds/run.sh")
        self.assertEqual(run.returncode, 0, run.stdout[-2000:] + run.stderr[-2000:])
        self.assertIn("0 failed", run.stdout)

    def test_tournament_game_index(self):
        run = _run("native/tools/mp/tourn_bounds/run.sh")
        self.assertEqual(run.returncode, 0, run.stdout[-2000:] + run.stderr[-2000:])
        self.assertIn("0 failed", run.stdout)


if __name__ == "__main__":
    unittest.main()
