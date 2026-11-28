"""tools15: gxp duplicate check, network namespace wrapper, public_scan, env file lint (host only: no emulator, no console)."""
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

try:
    import pytest
except ModuleNotFoundError:
    import unittest
    raise unittest.SkipTest("pytest not installed (run with pytest)")

ROOT = pathlib.Path(__file__).resolve().parent.parent
PY = [sys.executable, "-I"]


def run(args, **kw):
    return subprocess.run(args, capture_output=True, text=True, timeout=120, **kw)


def mutant(src, old, new, tmp, name=None):
    """a copy of a script with one edit; fails the test if the edit did not apply"""
    text = pathlib.Path(src).read_text()
    assert old in text, "mutant edit does not apply: %r" % old
    out = pathlib.Path(tmp) / (name or pathlib.Path(src).name)
    out.write_text(text.replace(old, new, 1))
    return out


# ---------------------------------------------------------------- 1. gxp duplicates
GXP = ROOT / "tools/harvest/check_gxp_dups.py"


def cache(tmp, name, files):
    for rel, data in files.items():
        p = pathlib.Path(tmp) / name / "874a13e0/v3" / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    return str(pathlib.Path(tmp) / name)


def test_gxp_identical_names_pass_and_different_bytes_fail(tmp_path):
    a = cache(tmp_path, "a", {"f/3A/AB.gxp": b"same", "v/01/CD.gxp": b"only a"})
    b = cache(tmp_path, "b", {"f/3A/AB.gxp": b"same", "f/3A/EF.gxp": b"only b"})
    r = run(PY + [str(GXP), a, b])
    assert r.returncode == 0 and "1 seen in more than one cache, 0 conflicts" in r.stdout
    c = cache(tmp_path, "c", {"f/3A/AB.gxp": b"DIFFERENT"})
    r = run(PY + [str(GXP), a, b, c])
    assert r.returncode == 1 and "CONFLICT" in r.stdout and "AB.gxp" in r.stdout


def test_gxp_missing_dirs_and_usage(tmp_path):
    assert run(PY + [str(GXP), str(tmp_path / "nope")]).returncode == 0
    assert run(PY + [str(GXP)]).returncode == 2


def test_gxp_mutants_are_caught(tmp_path):
    a = cache(tmp_path, "a", {"f/3A/AB.gxp": b"x"})
    b = cache(tmp_path, "b", {"f/3A/AB.gxp": b"y"})
    assert run(PY + [str(GXP), a, b]).returncode == 1
    m = mutant(GXP, "if seen[rel][0] != d:", "if False:", tmp_path, "m1.py")
    assert run(PY + [str(m), a, b]).returncode == 0          # the mutant misses the conflict ...
    # ... and the real script does not: the test above is the gate. A mutant that compares names only must fail the same test
    m2 = mutant(GXP, "rel = os.path.relpath(p, root)", "rel = n", tmp_path, "m2.py")   # (a different folder layout hides a clash)
    a2 = cache(tmp_path, "a2", {"f/3A/AB.gxp": b"x"})
    b2 = cache(tmp_path, "b2", {"v/3A/AB.gxp": b"y"})
    assert run(PY + [str(m2), a2, b2]).returncode == 1       # (same short name, different bytes, wrongly merged) -> shows the layout matters
    assert run(PY + [str(GXP), a2, b2]).returncode == 0      # real: different relative paths are different shaders


def test_harvest_wires_the_check_after_every_route():
    h = (ROOT / "tools/harvest/harvest.sh").read_text()
    assert 'check_gxp_dups.py" "$OUT"/*/*/cache' in h and "gxp CONFLICT" in h
    assert "check_gxp_dups.py" in h.split("cp \"$SRC\"/harvest.sh", 1)[1].split("\n", 1)[0]   # the private copy carries it too


# ---------------------------------------------------------------- 5. network namespace
NETNS = ROOT / "tools/netns_run.sh"
HAS_UNSHARE = subprocess.run(["unshare", "-n", "true"], capture_output=True).returncode == 0


@pytest.mark.skipif(not HAS_UNSHARE, reason="no network namespaces here")
def test_netns_only_loopback_and_no_route():
    r = run(["bash", str(NETNS), "bash", "-c", "tail -n +3 /proc/net/dev | cut -d: -f1 | tr -d ' '; (exec 3<>/dev/tcp/1.1.1.1/443) 2>&1; echo rc=$?"])
    assert r.stdout.split() [0] == "lo" and "Network is unreachable" in r.stdout
    assert "rc=1" in r.stdout


@pytest.mark.skipif(not HAS_UNSHARE, reason="no network namespaces here")
def test_netns_loopback_works_inside():
    code = "import socket;a=socket.socket();a.bind(('127.0.0.1',0));a.listen(1);b=socket.socket();b.connect(a.getsockname());print('ok')"
    r = run(["bash", str(NETNS), "python3", "-c", code])
    assert r.stdout.strip() == "ok", r.stderr


def test_netns_fails_closed_without_unshare(tmp_path):
    fake = tmp_path / "unshare"
    fake.write_text("#!/bin/sh\nexit 1\n")
    fake.chmod(0o755)
    r = run(["bash", str(NETNS), "echo", "RAN"], env={**os.environ, "PATH": "%s:%s" % (tmp_path, os.environ["PATH"])})
    assert r.returncode == 125 and "RAN" not in r.stdout


def test_netns_opt_out_runs_directly():
    r = run(["bash", str(NETNS), "echo", "RAN"], env={**os.environ, "HV_NETNS": "0"})
    assert r.returncode == 0 and r.stdout.strip() == "RAN" and "NOT isolated" in r.stderr


def test_netns_mutants_are_caught(tmp_path):
    if not HAS_UNSHARE:
        pytest.skip("no network namespaces here")
    code = "import socket;a=socket.socket();a.bind(('127.0.0.1',0));a.listen(1);b=socket.socket();b.connect(a.getsockname());print('ok')"
    # mutant 1: loopback not brought up -> the loopback check above must fail
    m = mutant(NETNS, 'fcntl.ioctl(s, 0x8914, struct.pack("16sH22x", b"lo", 0x1 | 0x40))', "pass", tmp_path, "m1.sh")
    assert run(["bash", str(m), "python3", "-c", code]).stdout.strip() != "ok"
    # mutant 2: no namespace at all -> the run is not isolated: the namespace is the container's own (the same /proc/net/ns inode as the parent's)
    m2 = mutant(mutant(NETNS, "exec unshare -n -- bash -c", "exec bash -c", tmp_path, "m2a.sh"), 'fcntl.ioctl(s, 0x8914, struct.pack("16sH22x", b"lo", 0x1 | 0x40))', "pass", tmp_path, "m2.sh")
    own = os.readlink("/proc/self/ns/net")
    r = run(["bash", str(m2), "readlink", "/proc/self/ns/net"])
    # the mutant either shares the namespace, or (the container has an interface besides lo) the in-namespace interface check refuses it with 125
    assert r.stdout.strip() == own or r.returncode == 125
    real = run(["bash", str(NETNS), "readlink", "/proc/self/ns/net"])
    assert real.returncode == 0 and real.stdout.strip() not in ("", own)   # the real script never shares it


def test_emulator_scripts_use_the_wrapper():
    one = (ROOT / "tools/harvest/harvest_one.sh").read_text()
    assert "netns_run.sh" in one and "HV_NETNS" in one and 'HV_NETNS:-0' not in one     # default ON
    fg = pathlib.Path("/home/claude/s13/tools/final-gate.sh")
    if fg.exists():
        t = fg.read_text()
        assert "netns-run.sh" in t and "nice -n 10 $V/run.sh" in t
        assert t.index("netns-run.sh") < t.index("nice -n 10 $V/run.sh")


# ---------------------------------------------------------------- 3. public_scan
PS = ROOT / "tools/public_scan.py"


def tree(tmp, files):
    d = pathlib.Path(tmp) / "tree"
    for rel, data in files.items():
        p = d / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data if isinstance(data, bytes) else data.encode())
    return str(d)


def scan(*args, env=None):
    e = {k: v for k, v in os.environ.items() if k not in ("STRIKERS_PUBLIC_SCAN_PATTERNS", "PUBLIC_SCAN_STRICT")}
    e.update(env or {})
    return run(PY + [str(PS), *args], env=e)


CLEAN = {"src/a.cpp": "int main(){return 0;}\n", "README.md": "hello\n", "sce_sys/icon0.png": b"\x89PNG"}
# secrets are built from pieces so this file itself holds none
TOK = "ghp_" + "a1B2c3D4e5F6g7H8i9J0k1L2m3N4o5P6q7R8"
CF = "account_id = \"" + "0123456789abcdef" * 2 + "\""
BAD = {
    "libshacccg": ("sce_module/libshacccg.suprx", b"x"),
    "firmware": ("fw/PSP2UPDAT.PUP", b"x"),
    "game data (disc image)": ("data/game.iso", b"x"),
    "game data (extension)": ("data/stage.szs", b"x"),
    "game data (disc system file)": ("x/opening.bnr", b"x"),
    "HD pack": ("data/hdtex.pack", b"x"),
    "Nintendo art path": ("art/Nintendo/logo.png", b"x"),
    "GitHub token": ("notes.txt", "token " + TOK + "\n"),
    "Cloudflare account id": ("wrangler.toml", CF + "\n"),
    "private key": ("k.pem", "-----BEGIN " + "RSA PRIVATE KEY-----\n"),
    "secret assignment": ("c.env", "API_TOKEN=" + "Zx9" * 10 + "\n"),
    "claude.ai session link": ("n.md", "see https://claude" + ".ai/code/session_" + "01Abc" * 3 + "\n"),
}


def test_scan_clean_tree_passes(tmp_path):
    r = scan("--tree", tree(tmp_path, CLEAN), "--no-git")
    assert r.returncode == 0 and "CLEAN" in r.stdout and "WARNING" in r.stdout     # (no personal list: said so)


@pytest.mark.parametrize("rule", sorted(BAD))
def test_scan_finds_each_rule(tmp_path, rule):
    rel, data = BAD[rule]
    r = scan("--tree", tree(tmp_path, {**CLEAN, rel: data}), "--no-git")
    assert r.returncode == 1 and rule in r.stdout, r.stdout
    assert TOK not in r.stdout                                                      # a secret is masked in the report


def test_scan_package_names_and_content(tmp_path):
    z = tmp_path / "p.vpk"
    with zipfile.ZipFile(z, "w") as zf:
        zf.writestr("eboot.bin", b"ok")
        zf.writestr("sce_module/libshacccg.suprx", b"x")
        zf.writestr("shadercache/x.pack", b"AIza" + b"A" * 35)
    r = scan("--package", str(z))
    assert r.returncode == 1 and "libshacccg" in r.stdout and "Google API key" in r.stdout and "p.vpk!" in r.stdout
    z2 = tmp_path / "q.vpk"
    with zipfile.ZipFile(z2, "w") as zf:
        zf.writestr("eboot.bin", b"ok")
        zf.writestr("sce_sys/icon0.png", b"\x89PNG")
    assert scan("--package", str(z2)).returncode == 0


def test_personal_patterns_from_a_file_outside_and_never_printed(tmp_path):
    t = tree(tmp_path, {**CLEAN, "docs/x.md": "written by Jane Q. Example\n", "tools/y.py": "# mail: someone@example.invalid\n"})
    pat = tmp_path / "outside" / "pats.txt"
    pat.parent.mkdir()
    pat.write_text("# my names\nJane Q. Example\nre:\\bsomeone@[a-z.]+\nnobody-here\n")
    r = scan("--tree", t, "--no-git", env={"STRIKERS_PUBLIC_SCAN_PATTERNS": str(pat)})
    assert r.returncode == 1 and "pattern #2" in r.stdout and "pattern #3" in r.stdout          # list line numbers
    assert "Jane" not in r.stdout and "example.invalid" not in r.stdout and "nobody-here" not in r.stdout
    assert "3 personal patterns" in r.stdout
    pat2 = tmp_path / "outside" / "p2.txt"
    pat2.write_text("something else entirely\n")
    assert scan("--tree", t, "--no-git", "--patterns", str(pat2)).returncode == 0


def test_personal_hash_pattern(tmp_path):
    import hashlib
    t = tree(tmp_path, {**CLEAN, "art/pic.png": b"nintendo art bytes"})
    pat = tmp_path / "o" / "p.txt"
    pat.parent.mkdir()
    pat.write_text("sha256:%s\n" % hashlib.sha256(b"nintendo art bytes").hexdigest())
    r = scan("--tree", t, "--no-git", "--patterns", str(pat))
    assert r.returncode == 1 and "content hash is on the list" in r.stdout


def test_strict_needs_the_list_and_list_inside_the_tree_is_refused(tmp_path):
    t = tree(tmp_path, CLEAN)
    assert scan("--tree", t, "--no-git", "--strict").returncode == 3
    assert scan("--tree", t, "--no-git", env={"PUBLIC_SCAN_STRICT": "1"}).returncode == 3
    inside = pathlib.Path(t) / "pats.txt"
    inside.write_text("secretname\n")
    r = scan("--tree", t, "--no-git", "--patterns", str(inside))
    assert r.returncode == 2 and "inside the tree" in r.stdout


def test_git_identities_are_scanned(tmp_path):
    t = tree(tmp_path, CLEAN)
    g = lambda *a: subprocess.run(["git", "-C", t, *a], capture_output=True, text=True, check=True)
    g("init", "-q")
    g("add", "-A")
    g("-c", "user.name=Private Person", "-c", "user.email=pp@example.invalid", "commit", "-q", "-m", "x")
    pat = tmp_path / "o" / "p.txt"
    pat.parent.mkdir()
    pat.write_text("Private Person\n")
    r = scan("--tree", t, "--patterns", str(pat))
    assert r.returncode == 1 and "git history" in r.stdout
    assert scan("--tree", t, "--no-git", "--patterns", str(pat)).returncode == 0


def test_this_repository_is_clean():
    r = scan("--tree", str(ROOT), "--no-git")
    assert r.returncode == 0, r.stdout[-2000:]


def load_scanner():
    import importlib.util
    spec = importlib.util.spec_from_file_location("public_scan_mut", PS)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


@pytest.mark.parametrize("rule", sorted(BAD))
def test_scan_mutants_each_rule_removed_is_caught(rule):
    """the scanner with ONE rule taken out misses that case (so the case above is a real gate); the unmutated scanner finds it"""
    rel, data = BAD[rule]
    data = data if isinstance(data, bytes) else data.encode()
    def findings(mod):
        out = []
        mod.scan_name(rel, rel, out)
        mod.scan_data(rel, data, mod.Personal(None), out)
        return out
    assert any("'%s'" % rule in f for f in findings(load_scanner()))
    mut = load_scanner()
    mut.NAME_RULES = [r for r in mut.NAME_RULES if r[0] != rule]
    mut.CONTENT_RULES = [r for r in mut.CONTENT_RULES if r[0] != rule]
    assert not any("'%s'" % rule in f for f in findings(mut)), "removing rule %s changed nothing" % rule


def test_final_build_runs_the_scan():
    fb = pathlib.Path("/home/claude/s13/tools/final-build.sh")
    if not fb.exists():
        pytest.skip("coordinator script not here")
    t = fb.read_text()
    assert "public_scan.py" in t and "stop public_scan" in t


# ---------------------------------------------------------------- 4. env lint
LINT = ROOT / "tools/check_env_file.py"


def lint(tmp, text, *extra):
    f = pathlib.Path(tmp) / "env.txt"
    f.write_bytes(text if isinstance(text, bytes) else text.encode())
    return run(PY + [str(LINT), *extra, str(f)])


def test_lint_clean_file_passes(tmp_path):
    r = lint(tmp_path, "# comment\nMSC_FPS=60\n  MSC_WATCH=1  \nMSC_INPUT_SCRIPT=ux0:data/x.txt\n")
    assert r.returncode == 0, r.stdout


@pytest.mark.parametrize("text,needle", [
    (b"\xef\xbb\xbfMSC_FPS=60\n", "byte-order mark"),
    ("MSC_FPS 60\n", "no '='"),
    ("=60\n", "empty variable name"),
    ("MSC_FPS =60\n", "blank in the name"),
    ("MSC_FPS=60 MSC_WATCH=1\n", "second assignment"),
    ("MSC_FPS=60 # fast\n", "trailing"),
    ("MSC_FPS=\"60\"\n", "in quotes"),
    ("MSC_FPS=60\nMSC_FPS=30\n", "set again"),
    ("MSC_FPS_NOBODY_READS_THIS=1\n", "has no reader"),
    ("MSC_" + "X" * 60 + "=1\n", "name is"),
    ("".join("MSC_FPS_%d=1\n" % i for i in range(97)), "variables"),
    ("MSC_FPS=" + "9" * 17000 + "\n", "bytes"),
])
def test_lint_errors(tmp_path, text, needle):
    r = lint(tmp_path, text)
    assert r.returncode == 1 and needle in r.stdout, r.stdout[-400:]


def test_lint_reader_check_can_be_warning(tmp_path):
    r = lint(tmp_path, "MSC_FPS_NOBODY_READS_THIS=1\n", "--warn-only-readers")
    assert r.returncode == 0 and " W " in r.stdout


def test_lint_reader_check_mutant(tmp_path):
    m = mutant(LINT, "if name.strip() not in names and not name.strip().startswith(prefixes):", "if False:", tmp_path, "m.py")
    f = tmp_path / "e.txt"
    f.write_text("MSC_FPS_NOBODY_READS_THIS=1\n")
    assert run(PY + [str(m), "--root", str(ROOT), str(f)]).returncode == 0       # the mutant lets a typo through
    assert run(PY + [str(LINT), str(f)]).returncode == 1


def test_lint_reader_via_extra_file(tmp_path):
    root = tmp_path / "r"
    (root / "tools").mkdir(parents=True)
    (root / "native").mkdir()
    (root / "native/a.cpp").write_text('const char* k = "MSC_READ_ME";\n')
    (root / "tools/env_readers_extra.txt").write_text("MSC_BY_EXTRA  # built at run time\nMSC_FAMILY_*  # prefix\n")
    f = tmp_path / "e.txt"
    f.write_text("MSC_READ_ME=1\nMSC_BY_EXTRA=1\nMSC_FAMILY_ONE=2\n")
    assert run(PY + [str(LINT), "--root", str(root), str(f)]).returncode == 0
    f.write_text("MSC_READ_ME=1\nMSC_TYPO=1\n")
    assert run(PY + [str(LINT), "--root", str(root), str(f)]).returncode == 1


def test_repository_env_files_lint_clean_except_known():
    r = run(PY + [str(LINT), str(ROOT / "tools/console-1.2/env"), str(ROOT / "tools/console-1.2/env-fb"), str(ROOT / "tools/console-1.2/fastboot/env")])
    errs = [l for l in r.stdout.splitlines() if ": E " in l]
    # known: the generated kit file n-sweepbg0.txt sets MSC_HITCH_MS twice (the later wins); a NEW error must fail this test
    assert all("n-sweepbg0.txt" in l and "MSC_HITCH_MS set again" in l for l in errs), "\n".join(errs[:10])


# ---------------------------------------------------------------- 2/6. source-level checks (the C++ tests are renderer/tests/run_shaderkey_test.sh and tests/movie/run_delay.sh)
def test_shader_logs_carry_both_hashes():
    s = (ROOT / "renderer/src/vgx_backend_gl.cpp").read_text()
    assert "[pack key P=%016llX T=%016llX]" in s and "[shaderkey] picture" in s


def test_thp_counter_is_in_the_summary_line():
    s = (ROOT / "native/platform/sdk/src/thp_native.cpp").read_text()
    assert "software decodes over the stuck limit (counted only)" in s and "swSinceMs" in s
