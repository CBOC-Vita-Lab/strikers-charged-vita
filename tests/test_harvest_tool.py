"""tools/harvest/harvest.sh: argument handling and the rule that compiled shaders never go into the repository (no emulator is started)."""
import subprocess, pathlib, tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
H = ROOT / "tools" / "harvest" / "harvest.sh"


def run(*args):
    return subprocess.run(["bash", str(H), *args], capture_output=True, text=True, cwd=ROOT, timeout=60)


def test_no_arguments_prints_usage():
    r = run()
    assert r.returncode == 2 and "harvest.sh --vpk" in (r.stdout + r.stderr)


def test_out_inside_the_repository_is_refused():
    with tempfile.NamedTemporaryFile(suffix=".vpk") as v:
        r = run("--vpk", v.name, "--out", str(ROOT / "tools" / "harvest" / "out-x"))
        assert r.returncode == 2 and "inside the repository" in r.stdout
        assert not (ROOT / "tools" / "harvest" / "out-x").exists()   # refused before anything was made


def test_scripts_parse():
    for f in ("harvest.sh", "harvest_one.sh"):
        assert subprocess.run(["bash", "-n", str(ROOT / "tools" / "harvest" / f)]).returncode == 0
