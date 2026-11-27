"""tools/run_host_tests.sh: discovery, a passing script, an empty selection is not a pass."""
import subprocess, pathlib, tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
RUN = ROOT / "tools" / "run_host_tests.sh"


def test_lists_the_scripts():
    r = subprocess.run(["bash", str(RUN), "--list"], capture_output=True, text=True, cwd=ROOT)
    assert r.returncode == 0 and "renderer/tests/run_fxcomp_test.sh" in r.stdout
    assert len(r.stdout.split()) > 50


def test_runs_one_script_and_an_empty_selection_fails():
    with tempfile.TemporaryDirectory() as d:
        r = subprocess.run(["bash", str(RUN), "-f", "run_picture_math_test", "--out", d + "/a"], capture_output=True, text=True, cwd=ROOT)
        assert r.returncode == 0 and "PASS run_picture_math_test" in r.stdout, r.stdout
        r = subprocess.run(["bash", str(RUN), "-f", "no_such_test_xyz", "--out", d + "/b"], capture_output=True, text=True, cwd=ROOT)
        assert r.returncode != 0
