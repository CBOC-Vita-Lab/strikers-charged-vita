#!/usr/bin/env python3
"""Builds test_pause_settings.cpp and runs it; then runs it again against each mutant of the logic it guards (every mutant must make it fail)."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

MUTANTS = [
    # (file relative to platform/, old text, new text, what it breaks)
    ("sdk/net/np_pause_settings.h", "if (networkedSession) return false;", "", "offered in a LAN/online session"),
    ("sdk/net/np_pause_settings.h", "if (switchValue && switchValue[0] == '0') return false;", "", "the switch has no effect"),
    ("sdk/net/np_pause_settings.h", "if (!platformHasScreen) return false;", "", "offered where no screen exists"),
    ("sdk/net/np_pause_settings.h", "    if (networkedSession) return false;\n    if (switchValue && switchValue[0] == '0') return false;\n    return platformHasScreen;", "    if (switchValue && switchValue[0] == '0') return false;\n    return platformHasScreen;", "the OPTIONS button offered in a LAN/online session"),
    ("sdk/net/np_pause_settings.h", "    if (networkedSession) return false;\n    if (switchValue && switchValue[0] == '0') return false;\n    return platformHasScreen;", "    if (networkedSession) return false;\n    return platformHasScreen;", "the switch does not turn the OPTIONS button off"),
    ("sdk/net/np_pause_settings.h", "    if (switchValue && switchValue[0] == '0') return false;\n    return platformHasScreen;", "    if (switchValue && switchValue[0] == '0') return false;\n    return true;", "the OPTIONS button offered where no screen exists"),
    ("sdk/net/np_pause_settings.h", "return frontEnd ? 12 : 0;", "return 0;", "the front-end popup's closing press is not swallowed"),
    ("sdk/net/np_pause_settings.h", "return frontEnd ? 12 : 0;", "return 12;", "the pause path swallows presses too (changed from the game's behaviour)"),
    ("sdk/net/np_pause_settings.h", "return ownerGone && framesGone >= limit;", "return framesGone >= limit;", "a lock with a live owner is released"),
    ("sdk/net/np_pause_settings.h", "return ownerGone && framesGone >= limit;", "return ownerGone && framesGone >= limit + 300;", "a stale lock is waited for 300 frames more (the popup appears late)"),
    ("vita/src/np_settings.cpp", "    if (!changed) return 0;\n    int n = 0;", "    int n = 0;", "a dialog when nothing changed"),
    ("vita/src/np_settings.cpp", "if (ids[i] == kLeaveSave) return i;", "if (ids[i] == kLeaveSaveRestart) return i;", "the restart is lit first"),
    ("vita/src/np_settings.cpp", "        ids[n++] = kLeaveDiscard;\n        ids[n++] = kLeaveKeepEditing;\n    } else {", "        ids[n++] = kLeaveDiscard;\n    } else {", "no KEEP EDITING while running"),
    ("vita/src/np_settings.cpp", "        ids[n++] = kLeaveSave;\n        ids[n++] = kLeaveDiscard;\n        ids[n++] = kLeaveKeepEditing;\n    } else {", "        ids[n++] = kLeaveSave;\n        ids[n++] = kLeaveSaveRestart;\n        ids[n++] = kLeaveDiscard;\n        ids[n++] = kLeaveKeepEditing;\n    } else {", "a restart offered in a running match"),
    ("vita/src/np_settings.cpp", "    if (count <= 0) return 0;\n    return ((index + direction) % count + count) % count;", "    if (count <= 0) return 0;\n    return (index + direction) % count;", "a move that does not wrap backwards"),
    ("vita/src/np_settings.cpp", "        if (s.choice[o] != s.loaded[o] && !IsLive(o)) return true;", "        if (s.choice[o] != s.loaded[o]) return false;", "a change needs no restart"),
    ("vita/src/np_settings.cpp", "        ids[n++] = kLeaveSave;\n        ids[n++] = kLeaveDiscard;\n        ids[n++] = kLeaveKeepEditing;\n    } else {", "        ids[n++] = kLeaveDiscard;\n        ids[n++] = kLeaveSave;\n        ids[n++] = kLeaveKeepEditing;\n    } else {", "DON'T SAVE listed before SAVE"),
    # layout (verify9 F1-F3): the dialog's text keeps inside its box and the screen outside the box is dimmed
    ("vita/src/np_settings.cpp", '"Some changes apply the next time the game starts."', '"These settings apply when the game restarts. A match in progress would be lost."', "dialog text runs past the box"),
    ("vita/src/np_settings.cpp", '"Some changes apply the next time the game starts."', '"Some changes apply the next time the game starts. Sorry about that, really."', "dialog text inside the box's right margin"),
    ("vita/src/np_settings.cpp", "    if (note && !restart) Shadowed", "    if (!restart) Shadowed", "the note is drawn although no next-start row changed"),
    ("vita/src/np_settings.cpp", "    if (count <= 0) return;\n    DimScreen(px);", "    if (count <= 0) return;", "the running dialog leaves the list undimmed"),
    ("vita/src/np_settings.cpp", "Shade(px, 0, 0, kWidth, kHeight, 0xFF000000u, 170)", "Shade(px, 80, 0, 800, kHeight, 0xFF000000u, 170)", "the list shows through left and right of the box"),
    ("vita/src/np_settings.cpp", "void DrawLeaveConfirmation(uint32_t* px, int choice, bool classic) {\n    DimScreen(px);", "void DrawLeaveConfirmation(uint32_t* px, int choice, bool classic) {", "the start-up dialog leaves the list undimmed"),
    # set18: SMOOTH 60 is a choice of FRAME RATE; choosing it moves no other row
    ("vita/src/np_settings.cpp", "    s.choice[option] = choice;\n    s.touched[option] = true;\n}", "    s.choice[option] = choice;\n    s.touched[option] = true;\n    if (option == OptionIndex(\"FRAME RATE\") && choice == 0) s.choice[OptionIndex(\"STADIUM CROWD\")] = 1;\n}", "SMOOTH 60 moves another row"),
]


def build(vita, platform, out, mutated):
    """mutated: (directory holding the mutated file, its name) or None. The mutated copy comes first on the include path / replaces the source."""
    cmd = shlex.split(os.environ.get("CXX", "c++")) + ["-std=c++17", "-O0"]
    if mutated:
        cmd += ["-I" + str(mutated)]
    cmd += ["-I" + str(vita / "src"), "-I" + str(platform / "sdk/net")]
    source = vita / "src/np_settings.cpp"
    if mutated and (mutated / "np_settings.cpp").exists():
        source = mutated / "np_settings.cpp"
    cmd += [str(vita / "tests/test_pause_settings.cpp"), str(source), "-o", str(out)]
    return subprocess.run(cmd, capture_output=True, text=True)


def main():
    vita = Path(__file__).resolve().parents[1]
    platform = vita.parent
    ok = True
    with tempfile.TemporaryDirectory(prefix="pause-settings-") as t:
        t = Path(t)
        r = build(vita, platform, t / "base", None)
        if r.returncode:
            print(r.stderr)
            return 1
        r = subprocess.run([str(t / "base")], capture_output=True, text=True)
        print(r.stdout.strip())
        if r.returncode:
            print(r.stderr)
            ok = False
        for n, (rel, old, new, what) in enumerate(MUTANTS):
            folder = t / f"m{n}"
            folder.mkdir()
            original = (platform / rel).read_text()
            if old not in original:
                print(f"MUTANT {n} ({what}): pattern not found")
                ok = False
                continue
            (folder / Path(rel).name).write_text(original.replace(old, new, 1))
            r = build(vita, platform, t / f"bin{n}", folder)
            if r.returncode:
                print(f"MUTANT {n} ({what}): does not build\n{r.stderr[:300]}")
                ok = False
                continue
            r = subprocess.run([str(t / f"bin{n}")], capture_output=True, text=True)
            killed = r.returncode != 0
            print(f"mutant {n} ({what}): {'killed' if killed else 'SURVIVED'}")
            ok = ok and killed
    print("OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
