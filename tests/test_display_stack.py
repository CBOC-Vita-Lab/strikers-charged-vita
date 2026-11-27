"""tools/check_display_stack.py: the display-thread stack gate's parser and walk (synthetic disassembly, with mutants that must fail)."""
import subprocess, sys, pathlib

TOOL = pathlib.Path(__file__).resolve().parent.parent / "tools" / "check_display_stack.py"


def test_self_test_passes():
    r = subprocess.run([sys.executable, "-I", str(TOOL), "--self-test"], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "MUTANT: a 2600-byte buffer in the leaf is caught" in r.stdout


def test_no_elf_is_inconclusive_not_a_pass():
    r = subprocess.run([sys.executable, "-I", str(TOOL), "/nonexistent.elf"], capture_output=True, text=True)
    assert r.returncode == 2
