#!/usr/bin/env python3
"""Builds test_native_settings.cpp (the engine-free model of the native settings screen) and runs it; then runs it against each mutant of np_native_settings.cpp
(every mutant must make it fail)."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

TARGET = "sdk/net/np_native_settings.cpp"
MUTANTS = [
    # (old text, new text, what it breaks)
    ("    NpSettings::Step(g_state, o, dir < 0 ? -1 : 1);", "    NpSettings::Step(g_state, o, 1);", "the previous value is never reached"),
    ("    if (paren) *paren = 0;\n", "", "the value keeps its parenthesis"),
    ("    out = NpSettingsGuard::Stamped(out, NpBuildIdentity::Version(), NpBuildIdentity::BuildId());", "", "the saved file has no build stamp (differs from the pixel screen's)"),
    ("    NpSettings::Load(g_state, out.c_str());   // what is on disk", "    // what is on disk", "Changed() stays true after a save"),
    ("    if (!Save()) return false;\n    return NpNativeSettingsBackendRestart", "    return NpNativeSettingsBackendRestart", "SAVE AND RESTART does not save"),
    ("bool Changed() { return g_open && (NpSettings::Changed(g_state) || g_state.reset); }", "bool Changed() { return false; }", "unsaved steps are not reported"),
    ('{"LANGUAGE", {"LANGUAGE", "IDIOMA", "LANGUE"}, kRowOption}', '{"LANGUAGES", {"LANGUAGE", "IDIOMA", "LANGUE"}, kRowOption}', "a row names an option that does not exist"),
    ("    if (!LoadFromBackend()) g_open = false;\n}", "}", "DISCARD keeps the steps"),
    ("    if (IsAllSettingsRow(page, row)) return kAllSettings;\n", "", "ONLINE ACCOUNT does not open the ONLINE page"),
    ("    if (!NpNativeSettingsBackendWrite(out.c_str(), static_cast<int>(out.size()))) return false;\n", "    NpNativeSettingsBackendWrite(out.c_str(), static_cast<int>(out.size()));\n", "a failed write is reported as saved"),
    ("    return s && *s ? s : t.en;", "    return t.en;", "labels are never translated"),
    ("    if (!NpSettings::Serialize(g_state, out)) return false;\n", "    NpSettings::Serialize(g_state, out);\n    if (out.empty()) return false;\n    out = \"\";\n", "the model's text is not what Serialize makes"),
    ("    NpSettings::Load(g_state, text.c_str());\n    return true;", "    return true;", "Reload and Open do not read the file"),
    ("std::snprintf(both, sizeof both, \"%s: %s\", name, value);", "std::snprintf(both, sizeof both, \"%s %s\", name, value);", "the NAME: VALUE form is lost"),
    # 1.3 live19 / unlock rows
    ('    if (!NextStartChanged()) return Copy("", out, cap);\n', "", "the popup has no note when a next-start row changed"),
    ("    return !matchRunning && NextStartChanged() ? kActSaveRestart : kActSave;", "    return NextStartChanged() ? kActSaveRestart : kActSave;", "a restart offered in a running match"),
    ("bool NextStartChanged() { return g_open && NpSettings::Changed(g_state) && NpSettings::NextStartChanged(g_state); }", "bool NextStartChanged() { return g_open && NpSettings::Changed(g_state); }", "a live-only change counts as waiting for the next start"),
    ('{{"UNLOCK ALL CHARACTERS", {"ALL CHARACTERS", "TODOS PERSONAJES", "TOUS PERSONNAGES"}, kRowOption},', '{{"FRAME RATE", {"ALL CHARACTERS", "TODOS PERSONAJES", "TOUS PERSONNAGES"}, kRowOption},', "the first row of page one is not the unlock-characters row"),
    ("    return Copy(NpText::In(g_language, english), out, cap);\n}\n\nbool PauseRestartOffered", "    return Copy(english, out, cap);\n}\n\nbool PauseRestartOffered", "the new popup labels are not translated"),
    # 1.3 front19: Apply (the leave buttons) and the OPTIONS list
    ("    return !matchRunning && NextStartChanged() ? kActSaveRestart : kActSave;", "    return !matchRunning && NpSettings::Changed(g_state) ? kActSaveRestart : kActSave;", "SAVE AND START AGAIN offered from OPTIONS when no next-start row changed"),
    ("    case kActKeep: return kStay;", "    case kActKeep: return Save() ? kLeft : kOutSaveFailed;", "KEEP EDITING saves"),
    ("    case kActDiscard: Discard(); return kLeft;", "    case kActDiscard: Save(); Discard(); return kLeft;", "DON'T SAVE writes the steps"),
    ("    case kActSave: return Save() ? kLeft : kOutSaveFailed;", "    case kActSave: Save(); return kLeft;", "a failed SAVE lets the screen go"),
    ("        Restart();                                   // saves first; returns only when the system refused (or on the host)\n", "        Save();\n", "SAVE AND START AGAIN does not ask for the restart"),
    ("        return Changed() ? kOutSaveFailed : kLeft;      // still changed = the save itself failed", "        return kLeft;", "a failed SAVE AND START AGAIN lets the screen go"),
    # ux13 (U2): the question after SAVE in a match
    ("    return matchRunning && e && e[0] == '1' && NextStartChanged();", "    return e && e[0] == '1' && NextStartChanged();", "the restart question is offered outside a running match"),
    ("    return matchRunning && e && e[0] == '1' && NextStartChanged();", "    return matchRunning && NextStartChanged();", "the restart question is on without MSC_PAUSE_RESTART"),
    ("    return matchRunning && e && e[0] == '1' && NextStartChanged();", "    return matchRunning && e && e[0] == '1' && NpSettings::Changed(g_state);", "the restart question is asked when only a live row changed"),
    ("Pick(g_language, button == 1 ? kStartAgain : kNotNow)", "Pick(g_language, kNotNow)", "both buttons of the restart question say NOT NOW"),
    ("Copy(Pick(g_language, kRestartAsk), out, cap)", "Copy(kRestartAsk.en, out, cap)", "the restart question is not translated"),
    # set18: the game's menu holds every player setting; RESET; help; the merged and removed rows ("@S:" patches the table, np_settings.cpp)
    ('     {nullptr, {"RESET TO DEFAULTS", "RESTABLECER TODO", "RÉTABLIR PAR DÉFAUT"}, kRowReset},', '     {nullptr, {"RESET TO DEFAULTS", "RESTABLECER TODO", "RÉTABLIR PAR DÉFAUT"}, kRowAccount},', "RESET TO DEFAULTS opens the ONLINE page"),
    ("        NpSettings::ResetAll(g_state);\n", "", "RESET TO DEFAULTS changes nothing"),
    ("bool Changed() { return g_open && (NpSettings::Changed(g_state) || g_state.reset); }", "bool Changed() { return g_open && NpSettings::Changed(g_state); }", "a reset of rows already at their defaults is not saved (retired lines stay)"),
    ('     {"HD CHARACTERS", {"HD CHARACTERS", "PERSONAJES HD", "PERSONNAGES HD"}, kRowOption},', '     {nullptr, {nullptr, nullptr, nullptr}, kRowOption},', "a player setting (HD CHARACTERS) is missing from the game's menu"),
    ("    if (!g_open || !shown || bar < 0 || bar >= Rows(page)) return Copy(\"\", out, cap);", "    if (!g_open || bar < 0 || bar >= Rows(page)) return Copy(\"\", out, cap);", "the help line shows while the panel is not in place"),
    ('constexpr int kRowsOnPage[kPages] = {5, 4, 5, 4, 3};', 'constexpr int kRowsOnPage[kPages] = {5, 5, 5, 5, 4};', "an empty bar counts as a row"),
    ('    {{"CONNECTION", {"CONNECTION", "CONEXIÓN", "CONNEXION"}, kRowOption},', '    {{"CORE PLAN", {"CONNECTION", "CONEXIÓN", "CONNEXION"}, kRowOption},', "CORE PLAN (SAFE START only) is in the game's menu"),
    ("    return Copy(NpText::In(g_language, NpSettings::GetOption(o).help[0]), out, cap);", "    return Copy(NpSettings::GetOption(o).help[0], out, cap);", "the help line is not translated"),
    ('{"DRAWING ONLY", {"DRAWING", "GRÁFICOS", "AFFICHAGE"}},', "", "GAME FIXES' value does not fit 24 characters in French"),
    ('@S:{{"SMOOTH 60 (DEFAULT)", {}}, {"30 LOCKED", {{"MSC_FPS", "30"}}}}};', '@S:{{"SMOOTH 60 (DEFAULT)", {}}, {"30 LOCKED", {{"MSC_FPS", "30"}}}, {"45 FPS", {{"MSC_FPS", "45"}}}}};', "a 45 mode is offered"),
    ('@S:{"STICK ONLY", {{"MSC_TOUCH_SAVE", "0"}}}', '@S:{"STICK ONLY", {{"MSC_TOUCH_SAVE", "off"}}}', "the old TOUCH MEGA SAVE OFF does not read as STICK ONLY"),
    ('@S:    {"MSC_GAME_FIXES", "none",', '@S:    {"MSC_GAME_FIXES_X", "none",', "an obsolete value in the block is not ignored"),
    ('@S:{{"WITH MODERN", {}}, {"ON", {{"MSC_REAR_TOUCH", "1"}}}', '@S:{{"ON WHEN MODERN IS CHOSEN", {}}, {"ON", {{"MSC_REAR_TOUCH", "1"}}}', "REAR TOUCH's value does not fit 24 characters"),
    ('{"REAR TOUCH", {"REAR TOUCH", "TÁCTIL", "TACTILE"}, kRowOption},', '{"REPLAYS & CUT-SCENES", {"REPLAYS", "REPETICIÓN", "RALENTIS"}, kRowOption},', "REPLAYS instead of REAR TOUCH"),
]


def build(platform, out, folder):
    cmd = shlex.split(os.environ.get("CXX", "c++")) + ["-std=c++17", "-O0", "-I" + str(platform.parent), "-I" + str(platform / "vita/src"), "-I" + str(platform / "vita/include")]
    target = platform / TARGET
    table = platform / "vita/src/np_settings.cpp"
    if folder and (folder / "np_native_settings.cpp").exists():
        target = folder / "np_native_settings.cpp"
    if folder and (folder / "np_settings.cpp").exists():
        table = folder / "np_settings.cpp"
    cmd += [str(platform / "vita/tests/test_native_settings.cpp"), str(target), str(platform / "sdk/net/np_native_settings_host.cpp"),
            str(table), str(platform / "vita/src/np_settings_guard.cpp"), "-o", str(out)]
    return subprocess.run(cmd, capture_output=True, text=True)


def main():
    platform = Path(__file__).resolve().parents[2]
    ok = True
    with tempfile.TemporaryDirectory(prefix="native-settings-") as t:
        t = Path(t)
        r = build(platform, t / "base", None)
        if r.returncode:
            print(r.stderr[:2000])
            return 1
        r = subprocess.run([str(t / "base")], capture_output=True, text=True)
        print(r.stdout.strip())
        if r.returncode:
            print(r.stderr[:2000])
            ok = False
        original = (platform / TARGET).read_text()
        table_original = (platform / "vita/src/np_settings.cpp").read_text()
        for n, (old, new, what) in enumerate(MUTANTS):
            folder = t / f"m{n}"
            folder.mkdir()
            on_table = old.startswith("@S:")
            source = table_original if on_table else original
            old, new = (old[3:], new[3:]) if on_table else (old, new)
            if old not in source:
                print(f"MUTANT {n} ({what}): pattern not found")
                ok = False
                continue
            (folder / ("np_settings.cpp" if on_table else "np_native_settings.cpp")).write_text(source.replace(old, new, 1))
            r = build(platform, t / f"bin{n}", folder)
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
