#!/usr/bin/env python3
"""The 1.3 prune's mutants (owner, 15 Nov: "remove any settings that don't make sense anymore or aren't needed"). Builds test_settings.cpp as CI does and runs it; then once
per mutant of np_settings.cpp / np_settings_guard.cpp (a patched copy comes first on the include path; the test #includes the guard itself) and every mutant must make the test fail.

    python3 native/platform/vita/tests/run_settings_prune_tests.py        (CXX=clang++ SANITIZE=1 as CI)
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

SET = "vita/src/np_settings.cpp"
GUARD = "vita/src/np_settings_guard.cpp"
MUTANTS = [
    # (file relative to platform/, old text, new text, what it breaks)
    (SET, "if (wasInBlock || s.reset) {", "if (s.reset) {", "a retired line inside the screen's block is kept"),
    (SET, "if (wasInBlock || s.reset) {", "if (true) {", "a retired line outside the block (a kit or hand line) is dropped"),
    (SET, "if (wasInBlock || s.reset) {", "if (wasInBlock) {", "RESET TO DEFAULTS leaves a retired line"),
    (SET, "    s.reset = true;   // (Serialize then drops", "    s.reset = false;   // (Serialize then drops", "ResetAll does not mark the reset"),
    (SET, '{{"SMOOTH 60 (DEFAULT)", {}}, {"30 LOCKED", {{"MSC_FPS", "30"}}}}}', '{{"SMOOTH 60 (DEFAULT)", {}}, {"30 LOCKED", {{"MSC_FPS", "30"}}}, {"UP TO 60", {{"MSC_FPS_BETWEEN", "pace"}}}}}', "a removed key (MSC_FPS_BETWEEN) is owned by a row again"),
    (SET, '{"CRISP", {{"MSC_VITA_UPSCALE", "sharp"}}}}},', "}},", "a kept row (SHARPNESS) lost a choice"),
    (SET, '{{"OFF (DEFAULT)", {}}, {"ON", {{"MSC_DOF", "lazy"}}}}},', '{{"OFF (DEFAULT)", {}}, {"ON", {{"MSC_DOF", "lazy"}}}, {"ON (AS GAME)", {{"MSC_DOF", "1"}}}}},', "the removed DEPTH OF FIELD choice is back"),
    (SET, [('{"SAFE START", {"CORE PLAN", "VERSION",', '{"SAFE START", {"CORE PLAN", "PRIORITIZE PLAYERS", "VERSION",'),
           ('    {"HD CHARACTERS",\n', '    {"PRIORITIZE PLAYERS", {"a", "b"}, 0, {{"ON", {}}, {"OFF", {{"MSC_DYNRES_FG", "0"}}}}},\n    {"HD CHARACTERS",\n')], None, "a page shows a removed row (PRIORITIZE PLAYERS is back)"),
    (SET, '{"SAFE START", {"CORE PLAN",', '{"SYSTEM", {"CORE PLAN",', "a page is renamed (set18: SAFE START and ONLINE)"),
    # set18
    (SET, '{"MSC_LAN_INPUT_DELAY", nullptr},', "", "a retired key (MSC_LAN_INPUT_DELAY) is missing from the table"),
    (SET, '    {"MSC_VGX_MSAA", "0", "ANTI-ALIASING OFF', '    {"MSC_VGX_MSAA_X", "0", "ANTI-ALIASING OFF', "an ANTI-ALIASING OFF line of the screen's block is not ignored"),
    (SET, '    {"MSC_GAME_FIXES", "none",', '    {"MSC_GAME_FIXES_X", "none",', "an ORIGINAL GAME FIXES OFF line of the screen's block is not ignored"),
    (SET, "        else if (inBlock) {", "        else if (true) {", "an obsolete value outside the block (a kit or hand line) is ignored"),
    (SET, "        if (FindObsolete(p->name, p->value)) continue;", "", "a preset with a fixed size is never rewritten as the rows' lines"),
    (SET, '{"STICK ONLY", {{"MSC_TOUCH_SAVE", "0"}}}', '{"STICK ONLY", {{"MSC_MEGA_AIM", "stick"}}}', "the old TOUCH MEGA SAVE OFF is not read by the merged MEGA SAVE AIM"),
    (SET, '{"MSC_VITA_SHARPEN", "SHARPNESS"},', "", "a retired key (MSC_VITA_SHARPEN) is missing from the table"),
    (SET, '{"MSC_DYNRES", nullptr},', "", "a retired key (MSC_DYNRES) is missing from the table"),
    (SET, "const bool normalizePicture = selector && np_picture::Find(selector->c_str()) && SelectorFitsRows(s, vars, selector->c_str());", "const bool normalizePicture = selector && np_picture::Find(selector->c_str());", "a preset selector is rewritten without checking the rows"),
    (SET, "drop = !(row >= 0 && s.choice[row] < 0 && !s.touched[row]);", "drop = true;", "a companion variant (CUSTOM SHARPNESS) loses its line"),
    (SET, "{MSC_DOF=lazy", "{MSC_DOF=lazy", "(placeholder, skipped)"),
    (GUARD, '    {"NP_MENU_NAV", "0",', '    {"NP_MENU_NAV_X", "0",', "the NP_MENU_NAV guard rule is gone"),
    (GUARD, '    {"MSC_VITA_ANISO", "*",', '    {"MSC_VITA_ANISO_X", "*",', "the MSC_VITA_ANISO guard rule is gone"),
    (GUARD, '    {"MSC_FX_LOWRES_SCENES", "*",', '    {"MSC_FX_LOWRES_SCENES_X", "*",', "the MSC_FX_LOWRES_SCENES guard rule is gone"),
]
MUTANTS = [m for m in MUTANTS if "skipped" not in m[3]]


def build(vita, platform, out, folder):
    cmd = shlex.split(os.environ.get("CXX", "c++")) + ["-std=c++17", "-O0", "-pthread"]
    if os.environ.get("SANITIZE") == "1":
        cmd += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    if folder:
        cmd += ["-I" + str(folder)]
    source = folder / "np_settings.cpp" if folder and (folder / "np_settings.cpp").exists() else vita / "src/np_settings.cpp"
    cmd += ["-I" + str(vita / "src"), "-I" + str(platform / "sdk/include"), "-I" + str(platform / "sdk/net"), "-I" + str(vita / "include"),
            str(vita / "tests/test_settings.cpp"), str(source), str(vita / "src/vita_plan.cpp"), "-o", str(out)]
    return subprocess.run(cmd, capture_output=True, text=True)


def main():
    vita = Path(__file__).resolve().parents[1]
    platform = vita.parent
    ok = True
    with tempfile.TemporaryDirectory(prefix="settings-prune-") as t:
        t = Path(t)
        r = build(vita, platform, t / "base", None)
        if r.returncode:
            print(r.stderr[:2000])
            return 1
        r = subprocess.run([str(t / "base")], capture_output=True, text=True)
        print(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "")
        if r.returncode:
            print(r.stdout[-1500:], r.stderr[-500:])
            ok = False
        for n, (rel, old, new, what) in enumerate(MUTANTS):
            folder = t / f"m{n}"
            folder.mkdir()
            original = (platform / rel).read_text()
            pairs = old if isinstance(old, list) else [(old, new)]
            mutated = original
            for a, b in pairs:
                if mutated.count(a) < 1:
                    print(f"MUTANT {n} ({what}): pattern not found")
                    ok = False
                    break
                mutated = mutated.replace(a, b, 1)
            else:
                (folder / Path(rel).name).write_text(mutated)
            if not (folder / Path(rel).name).exists():
                continue
            r = build(vita, platform, t / f"bin{n}", folder)
            if r.returncode:
                print(f"MUTANT {n} ({what}): does not build\n{r.stderr[:300]}")
                ok = False
                continue
            r = subprocess.run([str(t / f"bin{n}")], capture_output=True, text=True)
            killed = r.returncode != 0
            print(f"mutant {n} ({what}): {'killed' if killed else 'SURVIVED'}")
            ok = ok and killed
    print("OK: %d mutants" % len(MUTANTS) if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
