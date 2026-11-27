#!/usr/bin/env python3
"""Plan against docs: every MSC_ key that native/platform/vita/src/vita_plan.cpp gives a default to must have a docs/TECHNICAL.md row
that says so, with the same value; and the code's own fallback (what the host harness and any build without the plan reads) is listed
beside it. Run from the repository root:

    python3 tools/check-plan-docs.py            list the mismatches, exit 1 when there is one
    python3 tools/check-plan-docs.py --all      also print every key with its plan value, doc state and code fallback
    python3 tools/check-plan-docs.py --code-on  keys added since 92f5bc6 (or --since REV) whose own source default is ON in every build while their row says off, or that have no row

Findings:
  NOROW    no TECHNICAL row names the key
  NOPLAN   a row exists but never says that the Vita plan / player build turns it on
  STALE    a row of the key's own says default off / proposed while the plan turns it on
  NOCOND   the key is a default only under a condition (LOCKED 60, PS TV, lab / player build) and the row does not name the condition
  VALUE    the row talks about the plan but the plan's value is not written in it
  DOCDEF   (info) the doc says "default `X`" for the code and the plan's value differs: normal, the plan is the player's default
Code fallback (info, never an error): the literal the source falls back to when nothing sets the key, found by pattern.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PLAN = ROOT / "native/platform/vita/src/vita_plan.cpp"
TECH = ROOT / "docs/TECHNICAL.md"
SRC_DIRS = [ROOT / "renderer", ROOT / "native"]

PLAN_WORDS = ("vita plan", "vita_plan", "player build", "vita player", "vita build", "plan default", "kspeed", "kquality", "on by default",
              "default on", "in the plan", "the plan", "player default", "vita default")


def plan_defaults():
    """{key: [(value, group), ...]}. Groups: plan (unconditional in every Vita build), lock60 (only with MSC_LOCK60), pstv (only on a PS TV),
    lab / player (one build type), threads (the plan C/A/S thread settings), set (a setenv beside the tables: the same as plan)."""
    text = PLAN.read_text()
    found = {}
    spans = []
    for m in re.finditer(r"static const (?:char\* const|Setting)\s+(k[A-Za-z0-9]+)\[\](?:\[2\])?\s*=\s*\{(.*?)\};", text, re.S):
        name = m.group(1)
        group = {"kLock60": "lock60", "kPadOnly": "pstv", "kLab": "lab", "kPlayer": "player", "kC": "threads", "kA": "threads", "kS": "threads"}.get(name, "plan")
        spans.append((m.start(), m.end()))
        for kv in re.finditer(r'\{\s*"(MSC_[A-Z0-9_]+)"\s*,\s*"([^"]*)"\s*\}', m.group(2)):
            found.setdefault(kv.group(1), []).append((kv.group(2), group))
    for m in re.finditer(r'(?:setenv|add)\(\s*"(MSC_[A-Z0-9_]+)"\s*,\s*"([^"]*)"', text):
        if any(a <= m.start() < b for a, b in spans):
            continue
        found.setdefault(m.group(1), []).append((m.group(2), "set"))
    overlap = ROOT / "native/platform/compat/np_overlap.cpp"
    if overlap.exists():
        for m in re.finditer(r'Number\(\s*"(MSC_[A-Z0-9_]+)"\s*,\s*NP_OVERLAP_ON\((\d+)\)', overlap.read_text()):
            found.setdefault(m.group(1), []).append((m.group(2), "set"))
    return found


def doc_rows():
    rows = []
    for line in TECH.read_text().splitlines():
        if line.startswith("|") and "`MSC_" in line:
            cells = line.replace("\\|", "\x00").split("|")
            first = cells[1] if len(cells) > 1 else ""
            second = cells[2] if len(cells) > 2 and cells[2].strip().startswith("`MSC_") else ""
            rows.append((set(re.findall(r"MSC_[A-Z0-9_]+", first + " " + second)), line))
    return rows


def paragraph_mentions(key):
    out = []
    for line in TECH.read_text().splitlines():
        if not line.startswith("|") and key in line:
            out.append(line)
    return out


def code_fallback(key):
    """Literals the source falls back to for the key: EnvInt("KEY", 0), getenv-based helpers with a numeric default."""
    pat = re.compile(r'"%s"\s*,\s*(-?[0-9][0-9.]*[fFuUlL]*|true|false)\s*[,)]' % re.escape(key))
    vals = set()
    for base in SRC_DIRS:
        for p in base.rglob("*"):
            if p.suffix not in (".cpp", ".h", ".c", ".inc") or "tests" in p.parts or "tools" in p.parts or p.name == "vita_plan.cpp":
                continue
            try:
                t = p.read_text(errors="replace")
            except OSError:
                continue
            if key not in t:
                continue
            for m in pat.finditer(t):
                vals.add(m.group(1).rstrip("fFuUlL"))
    return sorted(vals)


def value_written(row, key, value):
    v = re.escape(value)
    if re.search(r"%s=`?%s`?(?![A-Za-z0-9_.])" % (re.escape(key), v), row):
        return True
    if re.search(r"`%s`" % v, row):
        return True
    return False


CONDITION_WORDS = {"lock60": ("lock60", "locked 60"), "pstv": ("ps tv", "pstv", "vita tv"), "lab": ("lab",), "player": ("player",), "threads": ("plan", "guard")}


def default_cell_says(row, value):
    """A row whose default column starts with the value in backticks (`| `KEY` | `1` | ...`), or whose key cell is KEY=value, states that default."""
    cells = [c.strip() for c in row.replace("\\|", "\x00").split("|")]
    return any(c.startswith("`%s`" % value) or c.startswith("Vita `%s`" % value) for c in cells[2:4]) or any(re.search(r"`MSC_[A-Z0-9_]+=%s`" % re.escape(value), c) for c in cells[1:4])


def row_states(row, key, value, group):
    """True when this one row is consistent with the plan value for the key."""
    low = row.lower()
    if group in ("plan", "set"):
        if default_cell_says(row, value):
            return True
        if any(w in low for w in PLAN_WORDS) and value_written(row, key, value):
            return True
        # a row that never speaks of a default cannot contradict the plan
        return not re.search(r"default|proposed|off until|stays off", low)
    return any(w in low for w in CONDITION_WORDS[group]) or not re.search(r"default|proposed|off until|stays off", low)


def check_key(key, entries, rows):
    own = [r for names, r in rows if key in names]
    anyrow = own or [r for _, r in rows if key in r] or paragraph_mentions(key)
    if not anyrow:
        return "NOROW", False
    for value, group in entries:
        # the key must have at least one row that states the plan's value (or the condition) ...
        if group in ("plan", "set"):
            stated = any(default_cell_says(r, value) or (any(w in r.lower() for w in PLAN_WORDS) and value_written(r, key, value)) for r in anyrow)
        else:
            stated = any(any(w in r.lower() for w in CONDITION_WORDS[group]) for r in anyrow)
        if not stated:
            return ("NOPLAN" if group in ("plan", "set") else "NOCOND"), bool(own)
        # ... and no row of its own may still call it off
        for r in own:
            cell = r.replace("\\|", "\x00").split("|")[1]
            m = re.search(r"%s=([A-Za-z0-9+_.]+)" % re.escape(key), cell)
            if m and "\x00" not in cell and m.group(1) != value and not m.group(1).isdigit():
                continue   # a row for another value of the key (MSC_VITA_UPSCALE=cas): it may say off
            if not row_states(r, key, value, group):
                return "STALE", True
    return "ok", bool(own)


ON_PATTERNS = [
    re.compile(r'"(MSC_[A-Z0-9_]+)"\s*,\s*([1-9][0-9]*)\s*\)'),                                   # EnvInt("KEY", 1)
    re.compile(r'getenv\("(MSC_[A-Z0-9_]+)"\)[^;]*;\s*return\s*!\(\s*e\s*&&\s*e\[0\]\s*==\s*\'0\'\)'),   # on unless =0
]
OFF_WORDS = ("default `0`", "default 0", "default off", "off by default", "default: off", "default `off`", "`0` (off)", "default is `0`")


def new_keys(since):
    """MSC_ names that the source gained since a revision (default the 7 Nov base 92f5bc6)."""
    import subprocess
    d = subprocess.run(["git", "-C", str(ROOT), "diff", since, "HEAD", "--", "renderer", "native"], capture_output=True, text=True).stdout
    return set(re.findall(r'^\+.*?"(MSC_[A-Z0-9_]+)"', d, re.M)) - set(re.findall(r'^-.*?"(MSC_[A-Z0-9_]+)"', d, re.M))


def code_on_report(plan, rows, only=None):
    """Keys whose own source default is ON in every build (not only through the plan) while their row calls them off."""
    bad = []
    for base in SRC_DIRS:
        for p in base.rglob("*"):
            if p.suffix not in (".cpp", ".h", ".c", ".inc") or "tests" in p.parts or "tools" in p.parts or p.name == "vita_plan.cpp":
                continue
            t = p.read_text(errors="replace")
            for pat in ON_PATTERNS:
                for m in pat.finditer(t):
                    key = m.group(1)
                    if only is not None and key not in only:
                        continue
                    own = [r for names, r in rows if key in names]
                    low = " ".join(own).lower()
                    if not own and not any(key in r for _, r in rows):
                        bad.append((key, "NOROW", p.name))
                    elif not own:
                        continue   # named inside another switch's row: accepted
                    elif any(w in low for w in OFF_WORDS) and "`%s`" % (m.group(2) if m.lastindex and m.lastindex >= 2 else "1") not in low and not re.search(r"default[^|]{0,12}`?(1|on)`?|on by default|default on|code default `1`", low):
                        bad.append((key, "DOCOFF", p.name))
    return sorted(set(bad))


def main():
    show_all = "--all" in sys.argv
    if "--code-on" in sys.argv:
        rows = doc_rows()
        out = code_on_report(plan_defaults(), rows, new_keys("92f5bc6" if "--since" not in sys.argv else sys.argv[sys.argv.index("--since") + 1]))
        for key, state, name in out:
            print("%-7s %-40s %s" % (state, key, name))
        print("%d keys on in the source with no row or a row that says off" % len(out))
        return 1 if out else 0
    plan = plan_defaults()
    rows = doc_rows()
    problems = []
    for key in sorted(plan):
        entries = plan[key]
        state, own = check_key(key, entries, rows)
        fb = code_fallback(key)
        shown = ",".join("%s(%s)" % (v, g) for v, g in entries)
        # code fallback against the plan: only an unconditional plan value matters
        plan_vals = {v for v, g in entries if g in ("plan", "set")}
        disagree = bool(plan_vals) and bool(fb) and not (plan_vals & set(fb))
        if show_all:
            print("%-34s plan=%-26s doc=%-7s own_row=%-5s code_fallback=%-8s %s" % (key, shown, state, own, ",".join(fb) or "-", "DIFFERS" if disagree else ""))
        if state != "ok":
            problems.append((key, shown, state, fb))
    print("plan keys: %d, with a doc problem: %d" % (len(plan), len(problems)))
    for key, shown, state, fb in problems:
        print("%-7s %-34s plan=%-26s code_fallback=%s" % (state, key, shown, ",".join(fb) or "-"))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
