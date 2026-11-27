#!/usr/bin/env python3
"""Builds test_live_settings.cpp (the three live cells and what feeds them), runs it (in-process tests, then the first-read rule against the old getenv rule for every value
the old code accepted, in a fresh process each), then against each mutant of the sources (every mutant must make a check fail)."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

SOURCES = {  # name in the build -> path relative to native/
    "live_settings.h": "include/np/live_settings.h",
    "comfort.h": "include/np/comfort.h",
    "np_live_settings.cpp": "platform/compat/np_live_settings.cpp",
    "np_perfmode.cpp": "platform/compat/np_perfmode.cpp",
    "np_controls.h": "platform/sdk/input/np_controls.h",
    "np_native_settings.cpp": "platform/sdk/net/np_native_settings.cpp",
    "np_settings.cpp": "platform/vita/src/np_settings.cpp",
}
MUTANTS = [
    # (source, old text, new text, what it breaks)
    ("live_settings.h", "return (e && *e && *e != '0') ? 1 : 0;", "return (e && *e) ? 1 : 0;", "the first read takes 0 as on (LESS SHAKE and CROWD)"),
    ("live_settings.h", "return (e && *e) ? (*e != '0' ? 1 : 0) : -1;", "return (e && *e) ? 1 : -1;", "REAR TOUCH=0 in the file reads as on"),
    ("live_settings.h", '"MSC_REAR_TOUCH");', '"MSC_REAR");', "the first read of REAR TOUCH looks at the wrong key"),
    ("live_settings.h", 'id == NP_LIVE_CROWD ? "MSC_PERFORMANCE_MODE"', 'id == NP_LIVE_CROWD ? "MSC_PERFORMANCE"', "the first read of CROWD looks at the wrong key"),
    ("live_settings.h", "static int cells[NP_LIVE_CELLS] = {NP_LIVE_UNREAD, NP_LIVE_UNREAD, NP_LIVE_UNREAD};", "static int cells[NP_LIVE_CELLS] = {0, 0, -1};", "a cell that never reads the environment"),
    ("live_settings.h", "v = __atomic_compare_exchange_n(cell, &expected, fromEnvironment, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED) ? fromEnvironment : expected;", "v = fromEnvironment;", "a first read is not kept (the environment is read at every call)"),
    ("np_live_settings.cpp", "    if (id < 0) return 0;\n", "    if (id < 0) id = NP_LIVE_SHAKE;\n", "an unknown key moves a cell"),
    ("np_live_settings.cpp", ': -1;       // same rule as the first read', ': 0;       // same rule as the first read', "a default REAR TOUCH choice turns it off instead of following the layout"),
    ("np_live_settings.cpp", 'else if (!strcmp(key, "MSC_PERFORMANCE_MODE")) id = NP_LIVE_CROWD;', 'else if (!strcmp(key, "MSC_PERFORMANCE_MODE")) id = NP_LIVE_SHAKE;', "CROWD sets the shake cell"),
    ("np_live_settings.cpp", "    return 1;\n}", "    setenv(key, valueOrNull ? valueOrNull : \"\", 1);\n    return 1;\n}", "the setter calls setenv"),
    ("np_live_settings.cpp", "    __atomic_store_n(NpLiveCell(id), v, __ATOMIC_RELAXED);\n", "", "a Set does not store"),
    ("comfort.h", "return NpLiveShakeQuarter() ? 0.25f : 1.0f;", "return NpLiveShakeQuarter() ? 0.5f : 1.0f;", "the shake is not a quarter"),
    ("np_perfmode.cpp", "    return NpLiveCrowdHidden();", "    static int mode = -1;\n    if (mode < 0) mode = NpLiveCrowdHidden();\n    return mode;", "CROWD is read once again (a row not applied)"),
    ("np_controls.h", "    const int v = NpLiveRear();\n", "    static int v0 = -2;\n    if (v0 == -2) v0 = NpLiveRear();\n    const int v = v0;\n", "REAR TOUCH is read once again (a row not applied)"),
    ("np_native_settings.cpp", "    if (NpLiveSettingsSet) NpSettings::ApplyLive(", "    if (false) NpSettings::ApplyLive(", "a save does not apply the live rows"),
    ("np_native_settings.cpp", "    if (NpLiveSettingsSet) NpSettings::ApplyLive(g_state, [](const char* key, const char* value) { NpLiveSettingsSet(key, value); });   // the running game takes the live rows now (before Load: it compares with the old file)\n    NpSettings::Load(g_state, out.c_str());", "    NpSettings::Load(g_state, out.c_str());\n    if (NpLiveSettingsSet) NpSettings::ApplyLive(g_state, [](const char* key, const char* value) { NpLiveSettingsSet(key, value); });", "live rows applied after the reload (nothing differs any more)"),
    ("np_settings.cpp", 'return !std::strcmp(title, "STADIUM CROWD") || !std::strcmp(title, "REDUCED CAMERA SHAKE") || !std::strcmp(title, "REAR TOUCH");', 'return !std::strcmp(title, "STADIUM CROWD") || !std::strcmp(title, "REDUCED CAMERA SHAKE") || !std::strcmp(title, "REAR TOUCH") || !std::strcmp(title, "FRAME RATE");', "a next-start row applied live"),
    ("np_settings.cpp", 'return !std::strcmp(title, "STADIUM CROWD") || !std::strcmp(title, "REDUCED CAMERA SHAKE") || !std::strcmp(title, "REAR TOUCH");', 'return !std::strcmp(title, "STADIUM CROWD") || !std::strcmp(title, "REAR TOUCH");', "LESS SHAKE is not live"),
    ("np_settings.cpp", "if (!IsLive(o) || s.choice[o] == s.loaded[o] || s.choice[o] < 0", "if (!IsLive(o) || s.choice[o] < 0", "ApplyLive hands over rows that did not change"),
]
# The old rule of each reader, for the first read (what the code did before the cells): value of the environment -> expected cell value.
ENV_CASES = [None, "", "0", "1", "2", "00", "01", "yes", "off", " 1", "-1", "true", "0x", "10"]


def old_rule(kind, value):
    if kind == "rear":
        return (1 if value[0] != "0" else 0) if value else -1
    return 1 if value and value[0] != "0" else 0


def build(root, work, out, folder):
    native = root
    cmd = shlex.split(os.environ.get("CXX", "c++")) + ["-std=c++17", "-O0", "-DNP_NATIVE=1", "-pthread"]
    paths = {k: native / v for k, v in SOURCES.items()}
    if folder:
        for name in os.listdir(folder):
            paths[name] = folder / name
            if name.endswith(".h"):
                cmd += ["-I" + str(folder)]   # (not used: the headers are included by relative path; see below)
    # a mutated header is reached by copying the tree's three headers into the mutant folder with the same relative layout
    cmd += ["-I" + str(native / "include"), "-I" + str(native), "-I" + str(native / "platform/vita/src"), "-I" + str(native / "platform/vita/include")]
    cmd += [str(native / "platform/vita/tests/test_live_settings.cpp"), str(paths["np_live_settings.cpp"]), str(paths["np_perfmode.cpp"]), str(paths["np_native_settings.cpp"]),
            str(native / "platform/sdk/net/np_native_settings_host.cpp"), str(paths["np_settings.cpp"]), str(native / "platform/vita/src/np_settings_guard.cpp"), "-o", str(out)]
    return subprocess.run(cmd, capture_output=True, text=True)


def first_read_cases(binary):
    bad = 0
    for kind, key in (("shake", "MSC_REDUCED_MOTION"), ("crowd", "MSC_PERFORMANCE_MODE"), ("rear", "MSC_REAR_TOUCH")):
        for value in ENV_CASES:
            env = {k: v for k, v in os.environ.items() if not k.startswith("MSC_")}
            if value is not None:
                env[key] = value
            r = subprocess.run([str(binary), "read", kind], capture_output=True, text=True, env=env)
            got = r.stdout.strip()
            want = str(old_rule(kind, value))
            if got != want:
                print(f"FIRST READ {kind} {key}={value!r}: got {got!r}, the old rule gives {want}")
                bad += 1
    print(f"first-read rule: {3 * len(ENV_CASES)} cases, {bad} different from the old getenv rule")
    return bad == 0


def set_first(binary):
    """A Set that comes before the first read wins over the environment."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("MSC_")}
    return all(subprocess.run([str(binary), mode], capture_output=True, text=True, env=env).returncode == 0 for mode in ("setfirst", "once"))


def source_rules(native):
    """No reader of the three keys outside the header, and the Vita input reads the rear switch at every poll."""
    ok = True
    keys = ("MSC_REDUCED_MOTION", "MSC_PERFORMANCE_MODE", "MSC_REAR_TOUCH")
    for path in list((native / "src").rglob("*.cpp")) + list((native / "platform").rglob("*.cpp")) + list((native / "platform").rglob("*.h")) + list((native / "include").rglob("*.h")):
        if path.name in ("live_settings.h", "np_settings.cpp", "np_settings_guard.cpp", "np_settings_guard.h", "np_picture_presets.h", "vita_plan.cpp") or "/tests/" in str(path) or "/tools/" in str(path):
            continue
        text = path.read_text(errors="ignore")
        for key in keys:
            if f'getenv("{key}")' in text:
                print(f"SOURCE RULE: {path} reads {key} with getenv")
                ok = False
    vita_input = (native / "platform/vita/src/np_vita_input.cpp").read_text()
    if "static const bool rearEnabled" in vita_input:
        print("SOURCE RULE: np_vita_input.cpp reads the rear switch once (static const)")
        ok = False
    print("source rules:", "ok" if ok else "FAILED")
    return ok


def main():
    native = Path(__file__).resolve().parents[3]
    ok = True
    with tempfile.TemporaryDirectory(prefix="live-settings-") as t:
        t = Path(t)
        r = build(native, t, t / "base", None)
        if r.returncode:
            print(r.stderr[:3000])
            return 1
        r = subprocess.run([str(t / "base")], capture_output=True, text=True)
        print(r.stdout.strip())
        if r.returncode:
            print(r.stderr[:2000])
            ok = False
        ok = first_read_cases(t / "base") and ok
        if not set_first(t / "base"):
            print("SET BEFORE FIRST READ: failed")
            ok = False
        ok = source_rules(native) and ok
        for n, (name, old, new, what) in enumerate(MUTANTS):
            source = (native / SOURCES[name]).read_text()
            if old not in source:
                print(f"MUTANT {n} ({what}): pattern not found")
                ok = False
                continue
            # the whole tree of the files the build uses is copied so that relative includes keep working
            folder = t / f"m{n}"
            for rel in set(SOURCES.values()):
                dst = folder / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                dst.write_text((native / rel).read_text())
            (folder / SOURCES[name]).write_text(source.replace(old, new, 1))
            r = build_tree(native, folder, t / f"bin{n}")
            if r.returncode:
                print(f"MUTANT {n} ({what}): does not build\n{r.stderr[:400]}")
                ok = False
                continue
            run = subprocess.run([str(t / f"bin{n}")], capture_output=True, text=True)
            killed = run.returncode != 0 or not first_read_cases_quiet(t / f"bin{n}") or not set_first(t / f"bin{n}")
            print(f"mutant {n} ({what}): {'killed' if killed else 'SURVIVED'}")
            ok = ok and killed
    print("OK" if ok else "FAILED")
    return 0 if ok else 1


def first_read_cases_quiet(binary):
    import io, contextlib
    with contextlib.redirect_stdout(io.StringIO()):
        return first_read_cases(binary)


def build_tree(native, folder, out):
    """Build with the mutated copy of the files in `folder` (same layout as native/); everything else comes from the real tree."""
    cmd = shlex.split(os.environ.get("CXX", "c++")) + ["-std=c++17", "-O0", "-DNP_NATIVE=1", "-pthread"]
    cmd += ["-I" + str(folder / "include"), "-I" + str(native / "include"), "-I" + str(native), "-I" + str(native / "platform/vita/src"), "-I" + str(native / "platform/vita/include")]
    # the test includes the headers by "np/..." (the mutant folder's include dir comes first) and np_controls.h by its path under platform/sdk: copy it where "platform/sdk/input/np_controls.h" resolves first
    cmd += ["-I" + str(folder)]
    cmd = cmd[:cmd.index("-I" + str(folder / "include"))] + ["-I" + str(folder), "-I" + str(folder / "include")] + cmd[cmd.index("-I" + str(folder / "include")) + 1:]
    cmd += [str(native / "platform/vita/tests/test_live_settings.cpp"), str(folder / SOURCES["np_live_settings.cpp"]), str(folder / SOURCES["np_perfmode.cpp"]),
            str(folder / SOURCES["np_native_settings.cpp"]), str(native / "platform/sdk/net/np_native_settings_host.cpp"), str(folder / SOURCES["np_settings.cpp"]),
            str(native / "platform/vita/src/np_settings_guard.cpp"), "-o", str(out)]
    return subprocess.run(cmd, capture_output=True, text=True)


if __name__ == "__main__":
    sys.exit(main())
