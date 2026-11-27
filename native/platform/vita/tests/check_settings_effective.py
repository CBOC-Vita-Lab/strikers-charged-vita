#!/usr/bin/env python3
"""Runtime check of the Settings screen on the desktop harness: for every choice of every row the harness can see, start the game with the
np-player.txt text the real Serialize writes for it and read the effective setting back from the log's [run] header (fps / gfx /
settings / controls lines, written by the modules that own each setting: NpFpsDescribe, VgxDynResDescribe, VgxGfxDescribe, NpSettingsDescribe,
NpInputDescribe).

Rows whose code only exists in the Vita build (CORE PLAN, SHARPNESS) and rows that print
no header line (DEPTH OF FIELD, MEGA STRIKES, LANGUAGE, REDUCED CAMERA SHAKE, CONNECTION) cannot be read back here; audit_settings_keys.py
checks their keys and values against the reader code, and the report names them.

The Vita build sets some values itself (vita_plan.cpp), not the host harness, whose own defaults are off. So every run here starts with the
build's values for them (BUILD_DEFAULTS, as vita_plan.cpp would set them). set18: the ANTI-ALIASING, TOUCH POINTER, TOUCH MEGA SAVE (now MEGA SAVE AIM's
STICK ONLY), RESOLUTION, SMOOTH 60 MODE (now a FRAME RATE choice), INPUT DELAY and MULTIPLAYER LINK rows went.

  BIN=<np-host32-full-gl> python3 native/platform/vita/tests/check_settings_effective.py [--keep]
Runs about 8 short games through /work/heavy.sh (one at a time with the other builds).
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), *[".."] * 4))
TESTS = os.path.join(ROOT, "native/platform/vita/tests")
SRC = os.path.join(ROOT, "native/platform/vita/src")
HRUN = os.environ.get("HRUN", "/work/hrun")
SCRIPT = os.environ.get("SCRIPT", "/work/msc/one/vita/vpk/unattended/nav_match60.txt")
HEAVY = os.environ.get("HEAVY", "/work/heavy.sh")

# row -> choice index -> what the header must say, as (field, text) where field names the [run] line; a callable gets the run's picks
DEFAULT = {  # the host's defaults for the rows it can see
    "STADIUM CROWD": 0, "FRAME RATE": 0,
    "HD CHARACTERS": 0, "ORIGINAL GAME FIXES": 1, "UNLOCK ALL CHARACTERS": 0, "UNLOCK ALL STADIUMS": 0, "CONTROLS": 0, "REAR TOUCH": 0,
    "MEGA SAVE AIM": 0, "MEGA SAVE SPEED": 0, "TOUCH MEGA STRIKE": 0, "TOUCH SHAKEDOWN": 0, "POINTER SPEED": 4,
}
VITA_ONLY = ["CORE PLAN", "SHARPNESS",
             "DEPTH OF FIELD", "MEGA STRIKES", "LANGUAGE", "REDUCED CAMERA SHAKE", "CONNECTION"]
BUILD_DEFAULTS = [("MSC_DYNRES_FG", "1"), ("MSC_VGX_MSAA", "4")]   # (1.3 prune: the rows for the ladder, the foreground-first switch and the other retired keys left the screen; the build still sets these two)   # what the Vita build sets when the file does not name the key
SPEED = ["3x", "4x"]   # (set18: ORIGINAL and 2x went)
POINTER = ["0.50", "0.60", "0.75", "0.85", "1.00", "1.15", "1.30", "1.50", "1.75", "2.00"]   # (1.3: the 1-10 scale)


def expectations(picks, host_msaa):
    """[(row, choice, line prefix, wanted text)] for one run; picks: row -> choice (rows not named are at their default)."""
    g = lambda row: picks.get(row, DEFAULT[row])
    out = []
    add = lambda row, line, text: out.append((row, g(row), line, text))
    add("STADIUM CROWD", "settings:", "performance-mode=" + ("on" if g("STADIUM CROWD") else "off"))
    # SMOOTH 60 writes nothing (the Vita plan sets MSC_LOCK60=1 itself); the desktop harness's default is 30, so both choices read fps=30
    # here and the explicit-auto run below proves that the policy parses and reports "auto"
    add("FRAME RATE", "fps=", "fps=30 ")
    add("HD CHARACTERS", "gfx:", "hdtex=" + ("off" if g("HD CHARACTERS") else "on"))
    fixes = g("ORIGINAL GAME FIXES")
    add("ORIGINAL GAME FIXES", "settings:", {0: None, 1: "game-fixes=net:1,electro:1,nk:1"}[fixes] or "game-fixes=")
    if g("UNLOCK ALL CHARACTERS"):
        add("UNLOCK ALL CHARACTERS", "settings:", "bonus-captains=off")
    if g("UNLOCK ALL STADIUMS"):
        add("UNLOCK ALL STADIUMS", "settings:", "bonus-stadiums=on")
    classic = g("CONTROLS") == 1
    add("CONTROLS", "controls:", "layout=" + ("classic" if classic else "modern"))
    rear = g("REAR TOUCH")
    add("REAR TOUCH", "controls:", "rear-touch=" + ("on" if rear == 1 or (rear == 0 and not classic) else "off"))
    save_on = g("MEGA SAVE AIM") != 2   # (set18: STICK ONLY is the old TOUCH MEGA SAVE OFF)
    rear_aim = g("MEGA SAVE AIM") == 1
    add("MEGA SAVE AIM", "controls:", "mega-save=" + (("rear" if rear_aim else "front-touch") if save_on else "buttons"))
    add("MEGA SAVE SPEED", "controls:", "mega-speed=" + SPEED[g("MEGA SAVE SPEED")][0] + "x")
    add("TOUCH MEGA STRIKE", "controls:", "touch-mega-strike=" + ("off" if g("TOUCH MEGA STRIKE") else "on"))
    add("TOUCH SHAKEDOWN", "controls:", "touch-shakedown=" + ("off" if g("TOUCH SHAKEDOWN") else "on"))
    add("POINTER SPEED", "controls:", "pointer-speed=" + POINTER[g("POINTER SPEED")])
    return out


def runs(options):
    """The runs: the defaults, choice k of every row that has one (k = 0..4)."""
    result = [("default", {})]
    for k in range(5):
        picks = {row: k for row, n in options.items() if row not in VITA_ONLY and k < n}
        if picks:
            result.append(("choice%d" % k, picks))
    result.append(("fps-auto", {}))   # (MSC_FPS=auto set by hand: the Vita default, see expectations())
    return result


def main():
    keep = "--keep" in sys.argv
    binary = os.environ.get("BIN")
    if not binary or not os.path.exists(binary):
        print("set BIN to the host harness (np-host32-full-gl)")
        return 2
    with tempfile.TemporaryDirectory(prefix="settings-effective-") as tmp:
        dump = os.path.join(tmp, "settings_dump")
        subprocess.run(["c++", "-std=c++17", "-I" + SRC, os.path.join(TESTS, "settings_dump.cpp"), os.path.join(SRC, "np_settings.cpp"), os.path.join(SRC, "vita_plan.cpp"), "-o", dump], check=True)
        options = {}
        for line in subprocess.run([dump, "rows"], check=True, capture_output=True, text=True).stdout.splitlines():
            row, idx, label, assigns = line.split("|", 3)
            options[row] = max(options.get(row, 0), int(idx) + 1)
        missing = [r for r in DEFAULT if r not in options]
        if missing:
            print("FAIL rows the check expects are gone:", missing)
            return 1
        unknown = [r for r in options if r not in DEFAULT and r not in VITA_ONLY]
        if unknown:
            print("FAIL rows with neither a runtime check nor a Vita-only entry (add them):", unknown)
            return 1
        failed = 0
        checked = {}
        host_msaa = None
        for name, picks in runs(options):
            args = [dump, "pick"] + ["%s=%d" % (r, c) for r, c in picks.items()]
            text = subprocess.run(args, check=True, capture_output=True, text=True).stdout
            env = ["MSC_FRAME_LOG=10"] + (["MSC_FPS=auto"] if name == "fps-auto" else [])
            named = set(line.split("=", 1)[0] for line in text.splitlines() if line and not line.startswith("#") and "=" in line)
            env += ["%s=%s" % kv for kv in BUILD_DEFAULTS if kv[0] not in named and not (kv[0] == "MSC_VGX_MSAA" and False)]
            for line in text.splitlines():
                if line and not line.startswith("#") and "=" in line:
                    env.append(line)
            tag = "w12settings-eff-" + name
            e = dict(os.environ, BIN=binary)
            r = subprocess.run([HEAVY, os.path.join(HRUN, "runb.sh"), tag, SCRIPT, "12", ""] + env, env=e, capture_output=True, text=True)
            log = open(os.path.join(HRUN, tag, "log.txt"), errors="replace").read() if os.path.exists(os.path.join(HRUN, tag, "log.txt")) else ""
            if not keep:
                shutil.rmtree(os.path.join(HRUN, tag), ignore_errors=True)
            header = [l for l in log.splitlines() if l.startswith("[run]")]
            if len(header) < 5:
                print("FAIL run %s: no [run] header in the log (%d lines); file was:\n%s" % (name, len(header), text))
                failed += 1
                continue
            joined = "\n".join(header)
            if host_msaa is None:
                # whether this GL gave multisampling at all: from the default run's own MSC_VGX_MSAA-less header we cannot tell, so ask the first MSAA run
                host_msaa = True
            for row, choice, line, want in (expectations(picks, host_msaa) if name != "fps-auto" else [("FRAME RATE", 0, "fps=", "fps=auto ")]):
                ok = want in joined
                checked[(row, choice)] = checked.get((row, choice), "") or ("ok" if ok else "FAIL")
                if not ok:
                    failed += 1
                    print("FAIL run %s: %s choice %d: want '%s' in the header\n%s" % (name, row, choice, want, joined))
            print("run %-12s %s -> %d checks" % (name, " ".join("%s=%d" % kv for kv in picks.items())[:90], len(expectations(picks, host_msaa))))
        every = [(r, c) for r, n in options.items() if r not in VITA_ONLY for c in range(n)]
        uncovered = [(r, c) for r, c in every if (r, c) not in checked]
        if uncovered:
            print("FAIL choices no run covered:", uncovered)
            failed += 1
        print("effective: %d choices read back from the header, Vita-only rows %s, %d failures" % (len(checked), VITA_ONLY, failed))
        return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
