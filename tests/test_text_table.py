"""The port's own texts in Spanish and French (1.3): native/platform/sdk/include/np/np_text_strings.tsv and the table made from it."""
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
NP = ROOT / "native/platform/sdk/include/np"
TSV = NP / "np_text_strings.tsv"
SPEC = re.compile(r"%(?:lu|[sdu])")


def entries():
    limit, cells, out = None, 0, []
    for number, line in enumerate(TSV.read_text(encoding="utf-8").splitlines(), 1):
        if line.startswith("#@limit"):
            limit, cells = int(line.split()[1]), 0
            continue
        if line.startswith("#@cells"):
            cells = int(line.split()[1])
            continue
        if not line.strip() or line.startswith("#"):
            continue
        out.append((number, (limit, cells), line.split("\t")))
    return out


class TextTableTest(unittest.TestCase):
    def test_rows_are_complete_and_unique(self):
        seen = {}
        for number, limit, cols in entries():
            self.assertEqual(len(cols), 3, f"line {number}: English, Spanish, French")
            self.assertTrue(all(c for c in cols), f"line {number}: an empty column")
            self.assertIsNotNone(limit[0], f"line {number}: no #@limit above it")
            self.assertNotIn(cols[0], seen, f"line {number}: {cols[0]!r} also on line {seen.get(cols[0])}")
            seen[cols[0]] = number

    def test_format_specifiers_match(self):
        for number, _, (en, es, fr) in entries():
            for t in (es, fr):
                self.assertEqual(SPEC.findall(en), SPEC.findall(t), f"line {number}: {t!r}")

    def test_only_safe_conversions(self):
        # the Vita's C library does not understand %zu %zd %lld %jd...: a later %s would read a number as a pointer. Only %s %d %u %lu.
        for number, _, cols in entries():
            for t in cols:
                for m in re.finditer(r"%(.?.?)", t):
                    spec = m.group(1)
                    if spec[:1].isalpha():
                        self.assertTrue(spec[0] in "sdu" or spec == "lu" or spec.startswith("lu"), f"line {number}: {t!r} has %{spec}")

    def test_every_character_has_a_glyph(self):
        # vita_font.h draws ASCII 32..126 and Latin-1 U+00A0..U+00FF; the game's popups take the same range
        for number, _, cols in entries():
            for t in cols:
                bad = [c for c in t if not (0x20 <= ord(c) <= 0x7E or 0xA0 <= ord(c) <= 0xFF)]
                self.assertFalse(bad, f"line {number}: {t!r} has {bad}")

    def test_lengths_fit_the_screens(self):
        for number, (limit, cells), (en, es, fr) in entries():
            for t in (es, fr):
                if limit:
                    self.assertLessEqual(len(t), limit, f"line {number}: {t!r} ({len(t)} characters)")
                # row names, choices, page names and table cells: the Settings columns hold 29 characters
                if cells and en.upper() == en and len(en) <= cells:
                    self.assertLessEqual(len(t), cells, f"line {number}: {t!r} ({len(t)} characters)")

    def test_controls_page_rule_lines_fit(self):
        # "INPUT: sentence (extras)" of np_control_table.h's sentence lines, every extra on: one 76-character line of the controls page
        table = {en: (es, fr) for _, _, (en, es, fr) in entries()}
        src = (ROOT / "native/platform/sdk/input/np_control_table.h").read_text(encoding="utf-8")
        lines = re.findall(r'\{[MCB], k\w+, "([^"]+)", 0, 0, "([^"]+)", \{("[^"]*"|0), ("[^"]*"|0)\}', src)
        self.assertGreaterEqual(len(lines), 6)
        for parts in lines:
            parts = [p.strip('"') for p in parts if p != "0"]
            for lang in (0, 1):
                text = [table.get(p, (p, p))[lang] for p in parts]
                whole = text[0] + ": " + "".join(text[1:])
                self.assertLessEqual(len(whole), 76, whole)

    def test_disc_message_replacements_fit(self):
        # native/platform/sdk/net/np_lan_text.h: the replacements of the disc's online messages are written in place, so each must be no longer than the
        # disc's own message in that language (the lengths are recorded in the header; no disc text is in the repo). Upper case, as the disc writes them.
        header = (ROOT / "native/platform/sdk/net/np_lan_text.h").read_text(encoding="utf-8")
        table = {en: (es, fr) for _, _, (en, es, fr) in entries()}
        patches = re.findall(r'\{0x([0-9a-f]+), "([^"]+)", \{(\d+), (\d+), (\d+)\}, (true|false), (true|false)\}', header)
        self.assertEqual(len(patches), 14 + 28)
        self.assertEqual(len({p[0] for p in patches}), len(patches))
        for _, key, en_len, es_len, fr_len, _, _ in patches:
            self.assertIn(key, table, f"{key!r}: no row in the table")
            self.assertLessEqual(len(key), int(en_len), key)
            es, fr = table[key]
            self.assertLessEqual(len(es), int(es_len), es)
            self.assertLessEqual(len(fr), int(fr_len), fr)
            for t in (key, es, fr):
                self.assertNotIn("NINTENDO", t.upper())
                self.assertNotRegex(t, r"[a-z]{3,}", f"{t!r}: the disc's online messages are upper case")

    @unittest.skipUnless(shutil.which("c++"), "no C++ compiler")
    def test_disc_message_patch_by_id(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = pathlib.Path(tmp) / "test_lan_text"
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-I", str(ROOT / "native"), str(ROOT / "native/platform/sdk/tests/test_lan_text.cpp"),
                            "-o", str(exe)], check=True)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_generated_table_is_current(self):
        r = subprocess.run([sys.executable, str(ROOT / "native/platform/vita/tools/gen_text.py"), "--check"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_every_settings_text_has_a_translation(self):
        # the rows, choices, help lines, pages and picture modes of np_settings.cpp's tables
        table = {en for _, _, (en, _, _) in entries()}
        src = (ROOT / "native/platform/vita/src/np_settings.cpp").read_text(encoding="utf-8")
        body = src[src.index("// ---- the options"):src.index("const char* const kBlockStart")]   # (set18: the two FRAME RATE tables stand just above kTable)
        body = re.sub(r"//[^\n]*|static_assert\([^;]*;", "", body)
        strings = set(re.findall(r'"((?:[^"\\]|\\.)*)"', body))
        assigned = set()
        for key, value in re.findall(r'\{"((?:MSC|NP)_[A-Z0-9_]+)", "([^"]*)"\}', body):
            assigned |= {key, value}
        missing = sorted(t for t in strings - assigned if t and t not in table)
        self.assertEqual(missing, [], "texts of the Settings tables with no Spanish/French line")

    @unittest.skipUnless(shutil.which("c++"), "no C++ compiler")
    def test_lookup_sentences_and_utf16(self):
        # native/platform/sdk/tests/test_text.cpp: np_text.h's functions on the real table
        with tempfile.TemporaryDirectory() as tmp:
            exe = pathlib.Path(tmp) / "test_text"
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-I", str(ROOT / "native"), str(ROOT / "native/platform/sdk/tests/test_text.cpp"),
                            "-o", str(exe)], check=True)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
