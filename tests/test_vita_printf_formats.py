"""No printf length modifier the Vita's C library does not understand in code that is compiled for the Vita.

The console's newlib is built without the C99 'z', 't', 'j' and 'hh' length modifiers (newlib.h: _WANT_IO_C99_FORMATS undefined; _WANT_IO_LONG_LONG is 1, so %ll is understood): the argument is skipped, so a later %s reads a number as a pointer and the
game crashes (the Galaxy port's console dumps, 11 Nov 2026), and every later number prints another argument's value. glibc takes them, so a host run never shows it.
Use a cast and %u / %lu / %llu (size_t is 32 bits on the console: (unsigned long) with %lu is right on both).

Scanned: renderer/src, renderer/vita, native/src, native/platform, native/include, renderer/include, excluding tests/ and tools/ folders (host only).
"""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
DIRS = ["renderer/src", "renderer/vita", "native/src", "native/platform", "native/include", "renderer/include"]
EXT = {".c", ".cc", ".cpp", ".h", ".hpp", ".inc", ".mm"}
SKIP_PARTS = {"tests", "tools"}
BAD = re.compile(r'%[-+ #0]*(?:\*|\d+)?(?:\.(?:\*|\d+))?(?:[ztj]|hh)[diouxXc]')


def string_literals(text):
    """The contents of every string literal outside comments, with the line number each starts on."""
    out = []
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            line += text.count("\n", i, j)
            i = j
        elif c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            i = j + 1
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append((line, text[i + 1:j]))
            i = j + 1
        else:
            i += 1
    return out


def offenders(text):
    return [(line, m.group(0)) for line, lit in string_literals(text) for m in BAD.finditer(lit)]


def vita_sources():
    for d in DIRS:
        for p in sorted((ROOT / d).rglob("*")):
            if p.suffix in EXT and p.is_file() and not (set(p.relative_to(ROOT).parts) & SKIP_PARTS):
                yield p


class VitaPrintfFormats(unittest.TestCase):
    def test_detector(self):
        self.assertEqual(offenders('printf("%zu %s", a, b); // "%zu" in a comment\n/* "%td" */ x("%08zx");'), [(1, "%zu"), (2, "%08zx")])
        self.assertEqual([f for _, f in offenders('f("%zd %jd %ju %td %.*zu %lu %llu %u %s");')], ["%zd", "%jd", "%ju", "%td", "%.*zu"])
        self.assertEqual([f for _, f in offenders('f("%hhu %hhd %02hhx %hu %lld");')], ["%hhu", "%hhd", "%02hhx"])
        self.assertEqual(offenders('f("%lu %llu %ld %x 100%% z");'), [])

    def test_no_unsupported_length_modifier(self):
        found = []
        for p in vita_sources():
            for line, spec in offenders(p.read_text(errors="replace")):
                found.append(f"{p.relative_to(ROOT)}:{line}: {spec}")
        self.assertEqual(found, [], "printf formats the Vita's libc does not understand (cast and use %u/%lu/%llu):\n" + "\n".join(found))


if __name__ == "__main__":
    unittest.main()
