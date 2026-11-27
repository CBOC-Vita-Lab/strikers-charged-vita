#!/usr/bin/env python3
"""Host test of the start check for Picture and Language changes (np_change_guard.cpp) with the address and undefined-behaviour sanitizers, then once per mutant
(startcheck19): a patched copy of np_change_guard.cpp comes first on the build and every mutant must make test_change_guard.cpp fail.

    python3 native/platform/vita/tests/run_change_guard_tests.py        (CXX=clang++ as CI)
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

SRC = "src/np_change_guard.cpp"
MUTANTS = [
    # (old text, new text, what it breaks)
    ("    if (fingerprint_ == 0) return state_;   // no watched line: the standard picture and language cannot be blamed, nothing is read or written\n", "", "a start on the standard lines is counted when the file has older numbers"),
    ("    if (fingerprint_ == rec.good) return state_;           // an ordinary start: nothing is written\n", "", "an ordinary start (lines equal the good ones) is counted and written"),
    ("    if (tries >= kFallbackAfter) {", "    if (tries >= kFallbackAfter + 1) {", "the fallback comes one start late"),
    ("    if (tries >= kFallbackAfter) {", "    if (tries >= kFallbackAfter - 1) {", "the fallback comes one start early"),
    ("    const int tries = rec.tried == fingerprint_ ? rec.tries : 0;", "    const int tries = rec.tries;", "a new fingerprint inherits the old one's failed starts"),
    ("    next.tries = tries + 1;\n    if (!Reserve(next))", "    next.tries = tries;\n    if (!Reserve(next))", "the first failed start is not counted"),
    ("    next.good = fingerprint_;\n    next.tried = fingerprint_;\n    next.tries = 0;\n    Reserve(next);", "    next.good = record_.good;\n    next.tried = fingerprint_;\n    next.tries = 0;\n    Reserve(next);", "the stable point does not move the good fingerprint"),
    ("    next.tries = 0;\n    Reserve(next);", "    next.tries = record_.tries;\n    Reserve(next);", "the stable point does not clear the tries"),
    ("    if (state_ != State::kReserved) return false;\n    if (files_.WriteGuard(Format(before_))) record_ = before_;\n    state_ = State::kCancelled;", "    if (state_ != State::kReserved) return false;\n    state_ = State::kCancelled;", "a cancel (settings screen, restart) leaves the start counted as failed"),
    ("    if (state_ != State::kCancelled) return;\n    Record next = before_;", "    return;\n    Record next = before_;", "a restart that failed is not counted"),
    ("void Guard::Failed() { Restore(); }", "void Guard::Failed() {}", "a failure after a cancel does not count the start"),
    ("    if (SwitchedOffByText(settings)) return state_;\n", "", "a kit run (MSC_BOOT_MATCH, MSC_INPUT_SCRIPT, MSC_CHANGE_GUARD=0 in the file) is judged"),
    ('    for (const char* kit : {"MSC_BOOT_MATCH", "MSC_INPUT_SCRIPT"}) {', '    for (const char* kit : {"MSC_BOOT_MATCH"}) {', "MSC_INPUT_SCRIPT does not switch the check off"),
    ("    if (switchedOff) return state_;\n", "", "the environment's switch is ignored"),
    ("    if (files_.ReadSettings(settings) == 2) return state_;", "    files_.ReadSettings(settings);", "a card error reading np-player.txt blocks or misjudges the start"),
    ("    if (got == 2) return state_;", "    (void)got;", "a card error reading np-change.txt is not a no-op"),
    ("    if (!Reserve(next)) return state_;   // could not write: the guard does nothing this start", "    Reserve(next);", "a failed write still goes on as reserved"),
    ("    if (state_ == State::kReserved) {\n        if (files_.WriteGuard(Format(before_))) record_ = before_;\n    }\n", "", "a second Begin keeps the first start's count"),
    ('"MSC_VGX_MSAA", "MSC_DOF"', '"MSC_DOF"', "a Picture key (MSC_VGX_MSAA) is not watched"),
    ('"MSC_LANGUAGE", "NP_LANG"', '"MSC_LANGUAGE", "NP_LANG", "MSC_PLAN"', "the way out (MSC_PLAN) is watched"),
    ('"MSC_LANGUAGE", "NP_LANG"', '"NP_LANG"', "the Language row is not watched"),
    ("        out[line.substr(0, eq)] = line.substr(eq + 1);", "        out.emplace(line.substr(0, eq), line.substr(eq + 1));", "the first line of a key wins (the loader's last one does)"),
    ("    if (lines.empty()) return 0;", "    if (lines.empty()) return 7;", "no watched line is not the standard fingerprint"),
    ("    if (state_ != State::kReserved && state_ != State::kCancelled) return;\n    Record next;", "    if (state_ != State::kReserved) return;\n    Record next;", "a cancelled start that gets through is not marked good"),
    # r16 F2 (SettingsSaved)
    ("    if (rec.tries == 0) return;                                      // no failed start stands: nothing is written\n", "", "a save with no failed start standing still writes the file"),
    ("    next.tried = 0;\n    next.tries = 0;\n    if (!files_.WriteGuard(Format(next))) return;", "    next.tried = 0;\n    if (!files_.WriteGuard(Format(next))) return;", "a save does not clear the failed-start count"),
    ("    next.tried = 0;\n    next.tries = 0;\n    if (!files_.WriteGuard(Format(next))) return;", "    next.tries = 0;\n    if (!files_.WriteGuard(Format(next))) return;", "a save leaves the fingerprint of the failed lines in the file"),
    ("    if (state_ == State::kReserved || state_ == State::kCancelled) { before_.tried = 0; before_.tries = 0; }", "", "a save during a counted start is undone by the next Cancel or Begin"),
    ("    if (files_.ReadGuard(text) != 0 || !Parse(text, rec)) return;   // no file, a card error or a damaged file: nothing to clear", "    files_.ReadGuard(text); Parse(text, rec);", "a missing or unreadable file is written over by a save"),
]
HEADER_MUTANTS = [
    ("bool Skips(const char* key) const { return state_ == State::kFallback && IsWatched(key); }", "bool Skips(const char* key) const { return state_ == State::kFallback; }", "the fallback also drops lines that are not Picture or Language"),
    ("bool Skips(const char* key) const { return state_ == State::kFallback && IsWatched(key); }", "bool Skips(const char* key) const { return false; }", "the fallback drops nothing"),
]


def build(vita, folder, out):
    cmd = shlex.split(os.environ.get("CXX", "c++")) + ["-std=c++17", "-O0", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    if folder:
        cmd += ["-I" + str(folder)]
    source = folder / "np_change_guard.cpp" if folder and (folder / "np_change_guard.cpp").exists() else vita / SRC
    cmd += ["-I" + str(vita / "src"), str(vita / "tests/test_change_guard.cpp"), str(source), "-o", str(out)]
    return subprocess.run(cmd, capture_output=True, text=True)


def main():
    vita = Path(__file__).resolve().parents[1]
    ok = True
    with tempfile.TemporaryDirectory(prefix="change-guard-") as temporary:
        t = Path(temporary)
        r = build(vita, None, t / "base")
        if r.returncode:
            print(r.stdout[-1500:], r.stderr[-1500:])
            sys.exit(1)
        r = subprocess.run([str(t / "base")], capture_output=True, text=True)
        print(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr[-500:])
        if r.returncode:
            print(r.stdout[-1500:], r.stderr[-500:])
            sys.exit(1)
        original = (vita / SRC).read_text()
        header = (vita / "src/np_change_guard.h").read_text()
        killed = 0
        for n, (old, new, what, *rest) in enumerate([(o, nw, w) for o, nw, w in MUTANTS] + [(o, nw, w, "h") for o, nw, w in HEADER_MUTANTS]):
            folder = t / f"m{n}"
            folder.mkdir()
            base = header if rest else original
            if base.count(old) != 1:
                print(f"MUTANT {n} ({what}): pattern found {base.count(old)} times")
                ok = False
                continue
            (folder / ("np_change_guard.h" if rest else "np_change_guard.cpp")).write_text(base.replace(old, new))
            b = build(vita, folder, folder / "bin")
            if b.returncode:
                print(f"MUTANT {n} ({what}): does not compile")
                print(b.stderr[-600:])
                ok = False
                continue
            run = subprocess.run([str(folder / "bin")], capture_output=True, text=True)
            if run.returncode == 0:
                print(f"MUTANT {n} SURVIVED: {what}")
                ok = False
            else:
                killed += 1
        print(f"{killed} of {len(MUTANTS) + len(HEADER_MUTANTS)} mutants killed")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
