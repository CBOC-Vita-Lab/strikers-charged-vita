"""Runs the console 1.2 kit's own tests (tools/console-1.2/test_kit.py) with the rest of the suite."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "console-1.2"))

from test_kit import *  # noqa: E402,F401,F403

if __name__ == "__main__":
    import unittest
    unittest.main()
