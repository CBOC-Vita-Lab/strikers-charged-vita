#!/usr/bin/env python3
"""check_env_file.py: lint console batch env files (np-player.txt / env.txt) the way the Vita build reads them (tools15; the idea of Galaxy's check_env_file.py, stricter).

    python3 -I tools/check_env_file.py [--root REPO] [--warn-only-readers] FILE|DIR ...        (a DIR = every *.txt below it)

The reader is LoadEnvFile in native/platform/vita/src/np_vita_main.cpp: leading and trailing blanks of a line are cut; a line is a comment only when its FIRST non-blank character is `#`
(a `#` later in a line is part of the value); a line needs `=` not at its start, else it is ignored without a word; the name is everything before the first `=`, blanks kept (so
`MSC_X =1` sets a variable called "MSC_X "); the value is the rest, no quotes handled; at most 96 variables (the rest ignored with a log note); a name of 48 characters or more is
refused; only the first 16383 bytes of the file are read (the lines after the last newline within them).
Errors (exit 1):
  BOM at the start; a line over the 16383-byte limit or the file over it; more than 96 variables; a name of 48 or more characters; an empty name; blanks inside or around a name;
  a line without `=` that is not a comment (ignored by the game: a typo that does nothing); a second NAME= assignment inside one value; a trailing ` # comment` in a value;
  the same variable set twice; a value in quotes; AND (the strict part) a name NO source reads: every key must have a reader, as audit_settings_keys.py READERS requires of a
  Settings key. A reader = the name as a string literal in a non-test source file under native/ renderer/ dependencies/vitaGL, or a key of READERS in
  native/platform/vita/tests/audit_settings_keys.py, or a line of tools/env_readers_extra.txt (a name or `PREFIX*`, with a reason after `#`: keys whose reader builds the name at run time,
  or keys read by the emulator / the console tools).
--warn-only-readers turns the reader check into warnings (the reader scan is a text search; a reader hidden in a macro needs an entry in env_readers_extra.txt).
"""
import os
import re
import sys

MAX_VARS, MAX_NAME, MAX_BYTES = 96, 47, 16383
SRC_DIRS = ("native", "renderer", "dependencies/vitaGL")
SRC_EXT = (".c", ".cc", ".cpp", ".h", ".hpp", ".inc", ".inl", ".mm")
LIT = re.compile(r'"([A-Za-z][A-Za-z0-9_]{2,})"')


def reader_names(root):
    names = set()
    for sub in SRC_DIRS:
        for dp, dn, fn in os.walk(os.path.join(root, sub)):
            dn[:] = [d for d in dn if d not in ("tests", "test", ".git")]
            for f in fn:
                if f.endswith(SRC_EXT):
                    try:
                        with open(os.path.join(dp, f), errors="replace") as fh:
                            names.update(LIT.findall(fh.read()))
                    except OSError:
                        pass
    audit = os.path.join(root, "native/platform/vita/tests/audit_settings_keys.py")
    if os.path.isfile(audit):
        with open(audit, errors="replace") as fh:
            names.update(re.findall(r'^\s+"([A-Z][A-Z0-9_]+)":\s*\(\[', fh.read(), re.M))
    extra, prefixes = set(), []
    ex = os.path.join(root, "tools/env_readers_extra.txt")
    if os.path.isfile(ex):
        with open(ex) as fh:
            for ln in fh:
                s = ln.split("#", 1)[0].strip()
                if s.endswith("*"):
                    prefixes.append(s[:-1])
                elif s:
                    extra.add(s)
    return names | extra, tuple(prefixes)


def lint(path, readers, warn_readers):
    """returns (errors, warnings, [messages])"""
    msgs, errs, warns = [], 0, 0

    def E(n, m):
        nonlocal errs
        errs += 1
        msgs.append("%s:%d: E %s" % (path, n, m))

    def W(n, m):
        nonlocal warns
        warns += 1
        msgs.append("%s:%d: W %s" % (path, n, m))

    with open(path, "rb") as f:
        raw = f.read()
    if raw.startswith(b"\xef\xbb\xbf"):
        E(1, "byte-order mark: the first name becomes '\\xef\\xbb\\xbfNAME' and is lost")
    if len(raw) > MAX_BYTES:
        E(1, "file is %d bytes: only the first %d are read, the lines after that are ignored" % (len(raw), MAX_BYTES))
    seen, nvars = {}, 0
    names, prefixes = readers
    for i, bl in enumerate(raw.split(b"\n"), 1):
        line = bl.decode("latin-1").strip(" \t\r")
        if not line or line[0] == "#":
            continue
        eq = line.find("=")
        if eq < 0:
            E(i, "no '=': the game ignores the line without a word (a typo that does nothing): %.40s" % line)
            continue
        if eq == 0:
            E(i, "empty variable name: the line is ignored")
            continue
        name, val = line[:eq], line[eq + 1:]
        if name != name.strip() or re.search(r"\s", name):
            E(i, "blank in the name %r: the variable is called exactly that" % name)
        if len(name) > MAX_NAME:
            E(i, "name is %d characters (limit %d): refused by the game" % (len(name), MAX_NAME))
        if re.search(r"\s(?:[A-Z][A-Z0-9]*_[A-Z0-9_]+)=", val):
            E(i, "a second assignment on the same line: the whole rest is the first one's value: %.50s" % val)
        if re.search(r"\s#", val):
            E(i, "a trailing '# comment' is part of the value, not a comment: %r" % val[-30:])
        if len(val) >= 2 and val[0] == val[-1] and val[0] in "\"'":
            E(i, "the value is in quotes: the quotes are part of the value: %s" % val[:30])
        nvars += 1
        if name in seen:
            E(i, "%s set again (line %d): the later one wins, but it counts twice toward the %d-variable limit" % (name, seen[name], MAX_VARS))
        seen[name] = i
        if name.strip() not in names and not name.strip().startswith(prefixes):
            (W if warn_readers else E)(i, "%s has no reader: no source file under %s names it (a typo, a retired key, or a reader hidden in a macro: tools/env_readers_extra.txt)" % (name, "/".join(SRC_DIRS[:2]) + "/..."))
    if nvars > MAX_VARS:
        E(1, "%d variables: the game reads %d and ignores the rest" % (nvars, MAX_VARS))
    return errs, warns, msgs


def main(argv):
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    warn_readers, paths, i = False, [], 0
    while i < len(argv):
        if argv[i] == "--root" and i + 1 < len(argv):
            root = os.path.abspath(argv[i + 1]); i += 2
        elif argv[i] == "--warn-only-readers":
            warn_readers = True; i += 1
        elif argv[i] in ("-h", "--help"):
            print(__doc__); return 2
        else:
            paths.append(argv[i]); i += 1
    if not paths:
        print(__doc__)
        return 2
    files = []
    for p in paths:
        if os.path.isdir(p):
            for dp, _dn, fn in os.walk(p):
                files += [os.path.join(dp, f) for f in sorted(fn) if f.endswith(".txt")]
        else:
            files.append(p)
    readers = reader_names(root)
    te = tw = bad = 0
    for p in sorted(files):
        e, w, msgs = lint(p, readers, warn_readers)
        for m in msgs:
            print(m)
        te += e; tw += w; bad += 1 if e else 0
    print("check_env_file: %d files, %d with errors (%d errors, %d warnings)" % (len(files), bad, te, tw))
    return 1 if te else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
