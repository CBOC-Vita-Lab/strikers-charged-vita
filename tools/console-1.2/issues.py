#!/usr/bin/env python3
"""One consolidated issue report of a console session: ISSUES.md, so every bug of a session can be fixed in one round.

    python3 issues.py <session dir> [--out ISSUES.md]

Reads what session.sh pulled (<run id>/log.txt, status.txt, dumps.txt, shots/, session-times.csv, lan-*/), plan.json and np_settings.cpp. Writes:
  1. the core session's before/after table (analyse.py --core);
  2. findings by severity, each with the run, route, scene, the evidence lines and a suspect switch:
     crash or hang; picture faults (automatic checks on the screenshots, plus the eye review list); performance regressions against 1.1 or against
     the current defaults; heap near its limit; shader compiles during play; online/LAN failures; failed route steps;
  3. switch verdicts from the A/B runs (tier 1 and 2) against the reference runs: KEEP DEFAULT, FLIP DEFAULT, HIDE, REMOVE or "no effect", with the
     Settings row each one touches (np_settings.cpp), so the morning fix round can apply them directly.
Standard library only. Nothing is sent anywhere.
"""
import argparse
import csv
import io
import re
import statistics
import struct
import sys
import zlib
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
import analyse as A  # noqa: E402

SEVERITIES = ("Crash or hang", "Picture faults", "Performance regressions", "Heap near its limit", "Shader compiles during play",
              "Online / LAN failures", "Failed route steps")

# The switch most likely behind a finding, by scene or symptom (docs/TECHNICAL.md). A suspect, not a diagnosis.
SUSPECTS = {
    "menu-main": "MSC_MENU_STILL (proposed), MSC_FX_GLOW, MSC_DYNRES_BLIND (menus have 17 scenes a frame)",
    "stadium-select": "MSC_MENU_STILL (proposed), MSC_FX_GLOW, MSC_DYNRES_BLIND",
    "walkout": "MSC_DOF (lazy), MSC_FX_LOWRES_SCENES, MSC_SHADOW_EVERY",
    "play": "MSC_VGX_SOFTXFORM, MSC_SHADOW_PLAY, MSC_CROWD_REFRESH, MSC_DYNRES_FG",
    "goal:nis": "MSC_SHADOW_EVERY, MSC_FPS_REPLAY, MSC_FX_LOWRES_SCENES", "goal:replay": "MSC_REPLAY_FARLOD, MSC_SHADOW_EVERY, MSC_FPS_REPLAY",
    "rep:nis": "MSC_SHADOW_EVERY, MSC_FX_LOWRES_SCENES", "rep:replay": "MSC_REPLAY_FARLOD, MSC_SHADOW_EVERY, MSC_CROWD_REPLAY",
    "mega-save": "MSC_FX_GOV_MEGA (proposed), MSC_FX_LOWRES, MSC_FX_LOWRES_MAXH, MSC_FX_THIN",
    "pipe-walkout": "MSC_STADIUM_HEAVY, MSC_FX_TIGHT (proposed)", "pipe-play": "MSC_STADIUM_HEAVY, MSC_FX_TIGHT / MSC_FX_GOV (proposed), MSC_FLOOR_BAKE",
    "lava-walkout": "MSC_FX_TIGHT (proposed), MSC_FX_HAZE_WIDE", "lava-play": "MSC_FX_GOV (proposed), MSC_CAMERA_ZOOM 1 load, item effects (MSC_FX_LOWRES)",
    "heap": "shader compiles outside the pack (MSC_VGX_FS_TRIM, MSC_VGX_PALMERGE), MSC_VGX_PALMERGE_MB, census or screenshot buffers",
    "compile": "a shader form outside pack 15: MSC_VGX_FS_TRIM, MSC_VGX_PALMERGE, MSC_VITA_UPSCALE, MSC_VGX_VS_TRIM",
    "edge": "MSC_VGX_VP_EXACT (exact viewports, vitaGL 0025)", "blank": "MSC_VGX_COPYINPLACE, MSC_VIEW_LAYERS, MSC_MENU_STILL (a stale or empty copy)",
    "static": "MSC_VGX_FS_TRIM (the final output clamp)", "crash": "the last switches the run's env file names; for the proposed build its 21 defaults (vita_plan.cpp kQuality)",
    "lan": "MSC_LAN_INPUT_DELAY, MSC_LAN_MODE; anything gated off in online play", "route": "the route script (gen_routes.py) or a menu change",
}


def suspect(key):
    return SUSPECTS.get(key) or SUSPECTS.get(key.split(":")[0] + "-play", "-")


# ---- screenshots: decode the console's PNGs (8-bit RGB, any zlib stream, filter 0 to 4) ----------------------------------------------------------
def decode_png(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    at, w = 8, None
    idat = b""
    while at + 8 <= len(data):
        n, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            if depth != 8 or ctype != 2:
                return None
        elif kind == b"IDAT":
            idat += body
        at += 12 + n
    if not w:
        return None
    try:
        raw = zlib.decompress(idat)
    except zlib.error:
        return None
    stride = w * 3
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - 3] if x >= 3 else 0
            b = prev[x]
            c = prev[x - 3] if x >= 3 else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    return w, h, rows


def luma_columns(w, h, rows):
    """Mean luminance of every column and every row (0..255)."""
    cols, rws = [0.0] * w, []
    for r in rows:
        s = 0.0
        for x in range(w):
            v = (r[3 * x] * 299 + r[3 * x + 1] * 587 + r[3 * x + 2] * 114) / 1000.0
            cols[x] += v
            s += v
        rws.append(s / w)
    return [c / h for c in cols], rws


def picture_checks(path):
    """-> list of (kind, text): 'blank' (one flat colour), 'edge' (the last column or row much darker than its neighbours: the 1-px line)."""
    img = decode_png(path.read_bytes())
    if not img:
        return []
    w, h, rows = img
    cols, rws = luma_columns(w, h, rows)
    out = []
    mean = sum(cols) / w
    spread = statistics.pstdev(rws + cols)
    if spread < 1.5:
        out.append(("blank", "flat picture (mean luminance %.0f, spread %.2f)" % (mean, spread)))
        return out
    for name, series in (("right", cols), ("bottom", rws)):
        if len(series) < 8:
            continue
        inner = [abs(series[i] - series[i - 1]) for i in range(2, len(series) - 2)]
        step = statistics.median(inner) + 1.0
        edge, next_ = series[-1], series[-2]
        if next_ - edge > max(12.0, 8 * step) and edge < 0.7 * next_:
            out.append(("edge", "the %s edge is a dark line: luminance %.0f against %.0f one pixel in (typical step %.1f)" % (name, edge, next_, step)))
    return out


# ---- Settings rows ---------------------------------------------------------------------------------------------------------------------------------
def settings_rows():
    """env key -> [Settings row names] from np_settings.cpp kTable (a row's block holds its choices' NAME=VALUE pairs)."""
    path = REPO / "native/platform/vita/src/np_settings.cpp"
    if not path.exists():
        return {}
    text = path.read_text(errors="replace")
    start = text.find("kTable[] = {")
    end = text.find("\n};", start)
    out, row = defaultdict(list), None
    for line in text[start:end].splitlines():
        m = re.match(r'^    \{"([^"]+)",\s*$', line)
        if m:
            row = m.group(1)
            continue
        if row:
            for key in re.findall(r'\{"(MSC_[A-Z0-9_]+|NP_[A-Z0-9_]+)",', line):
                if row not in out[key]:
                    out[key].append(row)
    return dict(out)


def build_defaults():
    """The build's default of each key vita_plan.cpp sets (kSpeed and kQuality): a run that names one of them tests turning it off."""
    path = REPO / "native/platform/vita/src/vita_plan.cpp"
    if not path.exists():
        return {}
    text = path.read_text(errors="replace")
    out = {}
    for table in ("kQuality[][2]", "kSpeed[][2]"):
        at = text.find(table)
        if at >= 0:
            block = text[at:text.find("};", at)]
            out.update(re.findall(r'\{"([A-Z0-9_]+)", "([^"]*)"\}', block))
    out.update(re.findall(r'add\("(MSC_[A-Z0-9_]+)", "([^"]*)"\)', text))     # (the ones set by a condition: MSC_VGX_MSAA)
    return out


# ---- the report ------------------------------------------------------------------------------------------------------------------------------------
def evidence(run, pattern, limit=3):
    rx = re.compile(pattern)
    return [l for l in run_lines(run) if rx.search(l)][:limit]


_LINES = {}


def run_lines(run_dir):
    if run_dir not in _LINES:
        log = run_dir / "log.txt"
        _LINES[run_dir] = log.read_text(errors="replace").splitlines() if log.exists() else []
    return _LINES[run_dir]


def times(root):
    out = {}
    path = Path(root) / "session-times.csv"
    if path.exists():
        with open(path, newline="") as f:
            for row in csv.DictReader(f):
                out[row.get("id", "")] = row
    return out


def verdict_for(entry, table, extras, ref_extras, defaults, rows_of):
    """The switch verdict of one A/B run: (verdict, effect text, settings rows)."""
    scenes = A.ROUTE_CLASSES.get(entry["route"], ())
    better, worse, effects = [], [], []
    for cls in scenes:
        row = table.get(cls, {}).get("rows", {}).get(entry["id"])
        if not row:
            continue
        marks = [k for k in ("fps", "p50", "p95") if row.get(k + "_v")]
        good = [k for k in marks if row[k + "_v"] == "*"]
        bad = [k for k in marks if row[k + "_v"] == "!"]
        if good:
            better.append(cls)
        if bad:
            worse.append(cls)
        ex, rx = extras.get(cls, {}), ref_extras.get(cls, {})
        late = ex.get("late")
        gpu, rgpu = ex.get("gpu_busy"), rx.get("gpu_busy")
        effects.append("%s %+.1f fps%s, p95 %+.1f ms%s%s%s" % (
            cls, row["fps_d"] or 0.0, row["fps_v"] or "", row["p95_d"] or 0.0, row["p95_v"] or "",
            (", late %.1f%%" % late) if late is not None else "", (", GPU %+.1f ms" % (gpu - rgpu)) if gpu is not None and rgpu is not None else ""))
    keys = [k for k, _ in entry["env"]]
    tests_off = any(k in defaults and dict(entry["env"])[k] != defaults[k] for k in keys)
    rows = sorted({r for k in keys for r in rows_of.get(k, [])})
    picture = entry["group"] in ("picture", "look", "stadium", "fill960", "menu")
    if not effects:
        return "NOT RUN / NO DATA", "-", rows
    if better and worse:
        v = "MIXED (look at the scenes: better in %s, worse in %s)" % (", ".join(better), ", ".join(worse))
    elif better:
        v = "FLIP DEFAULT" if tests_off or not picture else "FLIP DEFAULT (if the looks agree)"
    elif worse:
        if tests_off:
            v = "KEEP DEFAULT"
        else:
            v = "KEEP DEFAULT (the option only costs: keep it for its picture or REMOVE)" if picture else "REMOVE (pure negative)"
    else:
        v = "no effect" + (" -> HIDE the Settings row (keep the env key)" if rows and not picture else (" -> HIDE if the looks show no difference" if rows else ""))
    return v, "; ".join(effects), rows


def report(root, out_path):
    root = Path(root)
    plan = A.load_plan()
    by_id = {r["id"]: r for r in plan.get("runs", [])}
    t = times(root)
    findings = defaultdict(list)
    buf = io.StringIO()
    have_core = any((root / r).is_dir() for r in sum(A.core_runs(plan).values(), []))
    core_rows = A.core_summary(root, out=buf) if have_core else []
    runs = {}
    for rid, log in A.find_logs(root).items():
        runs[rid] = A.Run.parse(rid, Path(log).read_text(errors="replace"))

    def add(sev, rid, scene, what, lines=(), key=None):
        r = by_id.get(rid, {})
        findings[sev].append({"run": rid, "route": r.get("route", "-"), "version": r.get("version", "-"), "scene": scene, "what": what,
                              "evidence": [l[:220] for l in lines], "suspect": suspect(key or scene)})

    # ---- crash or hang; stability per run
    for rid, run in runs.items():
        d = root / rid
        status = (d / "status.txt").read_text().strip() if (d / "status.txt").exists() else t.get(rid, {}).get("status", "")
        dumps = (d / "dumps.txt").read_text().split() if (d / "dumps.txt").exists() else []
        if dumps:
            add(SEVERITIES[0], rid, "-", "crash dump written during the run: %s" % " ".join(dumps), evidence(d, r"FATAL|fatal|\[note |exception|abort"), "crash")
        if status and status != "done":
            last = [l for l in run_lines(d) if l.startswith("[")][-3:]
            add(SEVERITIES[0], rid, "-", "the run did not end (%s): crash, hang or stall" % status, last, "crash")
        elif run.problems and any(re.search(r"FATAL|CRASH|stall: no frame|\[fatal ", p) for p in run.problems):
            add(SEVERITIES[0], rid, "-", "fatal line in the log", run.problems[:3], "crash")
        s = A.stability(run)
        if s["heap_fill"] is not None and s["heap_fill"] >= A.HEAP_WARN:
            add(SEVERITIES[3], rid, "-", "C heap at %.0f%% of its block (peak %d KiB, extent %d KiB of %d)" % (100 * s["heap_fill"], s["heap_peak_kib"], s["heap_extent_kib"], s["heap_block_kib"]),
                evidence(d, r"\[mem\] C heap|C heap in use"), "heap")
        if s["play_compile_records"] and by_id.get(rid, {}).get("version", "").startswith("1.3"):
            add(SEVERITIES[4], rid, "play scenes", "%d frame-timing record(s) in play scenes hold a shader compile (pack 15 should leave none)" % s["play_compile_records"],
                evidence(d, r"shader compiles|\+[1-9]\d* (vertex|fragment) shaders"), "compile")
        # failed route steps
        steps = [l for l in run_lines(d) if re.search(r"\[script\].*(TIMEOUT|timed out|FAILED|unknown variable|bad line)", l)]
        if steps:
            add(SEVERITIES[6], rid, "-", "%d route step(s) timed out or failed" % len(steps), steps, "route")
    # missing scenes (the analysis cannot measure them)
    plan_, aruns, stats, dropped, table, refs = A.analyse(root)
    for rid in aruns:
        r = by_id.get(rid, {})
        if r.get("route") in A.EXPECT and r.get("role") not in ("look", "census", "settings") and aruns[rid].records:
            miss = [c for c in A.EXPECT[r["route"]] if c not in stats[rid]]
            if miss:
                add(SEVERITIES[6], rid, ", ".join(miss), "no usable window for %s (rerun candidate)" % ", ".join(miss), [], "route")

    # ---- pictures: automatic checks, missing pictures
    for d in sorted(p for p in root.iterdir() if p.is_dir() and (p / "shots").is_dir()):
        entry = by_id.get(d.name, {})
        want = entry.get("env", {}).get("MSC_VITA_SHOT", "")
        expected = len([x for x in want.split(",") if x]) if want else 0
        got = sorted((d / "shots").glob("shot_*.png"))
        if expected and len(got) < expected and (d / "status.txt").exists() and (d / "status.txt").read_text().strip() == "done":
            add(SEVERITIES[1], d.name, "-", "%d of %d screenshots came back" % (len(got), expected), [], "blank")
        for f in got:
            for kind, text in picture_checks(f):
                add(SEVERITIES[1], d.name, f.name, text + " (automatic check: confirm by eye in looks-core.html)", [], kind)

    # ---- performance regressions (core)
    for row in core_rows:
        for key, what in (("vs11", "1.3 current slower than 1.1"), ("vsprop", "1.3 proposed slower than 1.3 current")):
            if key in row and row[key][1] == "WORSE":
                a, b = (row["1.3"], row["1.1"]) if key == "vs11" else (row["1.3p"], row["1.3"])
                add(SEVERITIES[2], ",".join(a["runs"]), row["scene"], "%s: %.1f against %.1f fps (%+.1f, noise %s)" % (
                    what, a["fps"], b["fps"], a["fps"] - b["fps"], "%.2f" % row["noise_fps"] if row["noise_fps"] is not None else "unknown"), [], row["scene"])

    # ---- LAN legs
    for leg in ("lan-base", "lan-auto"):
        d = root / leg
        for side in ("host", "join"):
            log = d / ("%s.log.txt" % side)
            if log.exists():
                bad = [l for l in log.read_text(errors="replace").splitlines() if l.startswith("[mp-lan]") and re.search(r"DESYNC|sync error|queue overflow|malformed|connection lost", l, re.I)]
                if bad:
                    add(SEVERITIES[5], "%s/%s" % (leg, side), "LAN match", "%d problem line(s)" % len(bad), bad[:4], "lan")
                if "script exit" not in log.read_text(errors="replace"):
                    add(SEVERITIES[5], "%s/%s" % (leg, side), "LAN match", "the LAN script did not reach its end", [], "lan")

    # ---- switch verdicts (tier 1 and 2 A/B runs on 1.3)
    defaults, rows_of = build_defaults(), settings_rows()
    verdicts = []
    ref_extras = defaultdict(dict)
    for rid in refs:
        if rid in aruns:
            for cls, e in A.scene_extras(aruns[rid]).items():
                ref_extras[cls].setdefault("gpu", []).append(e.get("gpu_busy"))
    ref_mean = {cls: {"gpu_busy": A._median(v.get("gpu", []))} for cls, v in ref_extras.items()}
    for entry in plan.get("runs", []):
        if entry.get("tier") in (1, 2) and entry.get("role") == "ab" and entry.get("version") == "1.3" and entry["id"] in aruns:
            e = dict(entry, env=list(entry.get("env", {}).items()))
            v, effect, srows = verdict_for(e, table, A.scene_extras(aruns[entry["id"]]), ref_mean, defaults, rows_of)
            st = (root / entry["id"] / "status.txt")
            if st.exists() and st.read_text().strip() != "done":
                v = "REMOVE or fix (the run did not end: see Crash or hang)"
            verdicts.append((entry["id"], " ".join("%s=%s" % kv for kv in e["env"]), v, effect, ", ".join(srows) or "-"))

    # ---- write
    o = io.StringIO()
    w = lambda s="": o.write(s + "\n")
    w("# Strikers Charged Vita 1.3: console session issues")
    w()
    w("Session folder: `%s`. Runs with a log: %d. Generated by tools/console-1.2/issues.py; every number comes from the console logs." % (root.name, len(runs)))
    w()
    w("## Before / after (core session)")
    w()
    if core_rows:
        w("| scene | 1.1 fps / size | 1.3 fps / size | change | 1.3 proposed fps / size | proposed vs current |")
        w("| --- | --- | --- | --- | --- | --- |")
        for row in core_rows:
            cell = lambda a: ("%.1f / %s" % (a["fps"], A.fmt_size(a["size"]))) if a else "-"
            ch = lambda k: ("%+.1f fps %s" % (row[k][0], row[k][1])) if k in row else "-"
            w("| %s | %s | %s | %s | %s | %s |" % (row["scene"], cell(row["1.1"]), cell(row["1.3"]), ch("vs11"), cell(row["1.3p"]), ch("vsprop")))
        w()
        w("<details><summary>Full core output (p95, p99, late %, GPU busy, CPU lanes, hitches, stability)</summary>")
        w()
        w("```")
        w(buf.getvalue().rstrip())
        w("```")
        w("</details>")
    else:
        w("No core-session runs in this folder.")
    w()
    total = sum(len(v) for v in findings.values())
    w("## Findings by severity (%d)" % total)
    for sev in SEVERITIES:
        items = findings.get(sev, [])
        w()
        w("### %s (%d)" % (sev, len(items)))
        if not items:
            w()
            w("None.")
            continue
        for f in items:
            w()
            w("- **%s** (route %s, version %s, scene %s): %s" % (f["run"], f["route"], f["version"], f["scene"], f["what"]))
            w("  - suspect: %s" % f["suspect"])
            for line in f["evidence"]:
                w("  - `%s`" % line.replace("`", "'"))
    w()
    w("### Picture review by eye")
    w()
    w("Open `looks-core.html`: shadows and decals without static (FS_TRIM clamp), no 1-px edge line in the looksmall rows (exact viewports), glow present and not "
      "blown out (proposed: one level shorter), the mist of Pipeline Central, Spanish accents in the lookes rows and on the Settings pages. Add what you see here.")
    w()
    w("## Switch verdicts (A/B runs against the references)")
    w()
    w("Noise = 2 x the spread of the reference runs of the same route (d1/dmid/d2, megad1/megad2, pipe1/pipe2, rep1/rep2). A verdict needs a difference beyond it "
      "(the `*` better / `!` worse marks of analyse.py); otherwise \"no effect\". KEEP DEFAULT: the default helps. FLIP DEFAULT: the tested value is better. "
      "HIDE: no measurable effect and no picture value: take the row off Settings, keep the env key. REMOVE: pure negative or broken. Picture options are judged "
      "with the looks as well.")
    w()
    if verdicts:
        w("| run | tested value | verdict | effect against the references | Settings row |")
        w("| --- | --- | --- | --- | --- |")
        for rid, tested, v, effect, srow in verdicts:
            w("| %s | `%s` | %s | %s | %s |" % (rid, tested or "-", v, effect, srow))
    else:
        w("No A/B runs (tier 1 or 2) in this folder yet.")
    Path(out_path).write_text(o.getvalue())
    return findings, verdicts, core_rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("session")
    ap.add_argument("--out")
    ap.add_argument("--morning", action="store_true", help="write MORNING.md (the owner's page of the overnight session, morning.py) and looks-night.html")
    ap.add_argument("--pixels", action="store_true", help="with --morning: also diff the screenshots of each picture switch against the baseline's (slow: pure Python)")
    args = ap.parse_args()
    if args.morning:
        import morning
        import looks
        out = args.out or str(Path(args.session) / "MORNING.md")
        res = morning.write(args.session, out, settings_rows(), args.pixels, decode_png)
        try:
            text, n = looks.night_page(args.session, res.get("decisions", []))
            if n:
                (Path(args.session) / "looks-night.html").write_text(text)
        except Exception as exc:   # a broken picture page must not cost the morning page
            print("looks-night.html not written: %s" % exc, file=sys.stderr)
        print("wrote %s: %d decision(s), %d scene row(s), %d problem(s)" % (out, len(res.get("decisions", [])), len(res.get("rows", [])), len(res.get("problems", []))))
        return 0
    out = args.out or str(Path(args.session) / "ISSUES.md")
    findings, verdicts, rows = report(args.session, out)
    print("wrote %s: %d finding(s), %d switch verdict(s), %d core scene(s)" % (out, sum(len(v) for v in findings.values()), len(verdicts), len(rows)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
