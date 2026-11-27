#!/usr/bin/env python3
"""Contact sheet of the look runs: the same match moment in every run, side by side.

    python3 looks.py <session dir> [--base lookbase] [--out looks.html]
    python3 looks.py <session dir> --core            # looks-core.html: the core session's look runs side by side, 1.1 | 1.3 current | 1.3 proposed

<session dir> holds <run id>/shots/shot_<present>.png (what session.sh pulls for the look runs) and <run id>/log.txt. The look runs
are held at 30 fps (MSC_FPS=30) and take their pictures at fixed presents, so one present number is one match moment in every run.
Each row is one present, the first column is the base run (lookbase), the others the run under test; the label under a picture says
which scene the log's marks and frame-timing records put that present in. Writes one HTML page next to the pictures (relative links,
no scripts, no downloads).
"""
import argparse
import html
import re
import struct
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import analyse as A  # noqa: E402

SHOT = re.compile(r"shot_(\d+)\.png$")


def scene_at(run, present):
    """The scene class a present falls in: the mark interval first, else the frame-timing record that holds it."""
    intervals = run.mark_intervals()
    for rec in run.records:
        if rec["first_frame"] <= present <= rec["last_frame"]:
            return run.classify(rec, intervals)
    for a, b, name in intervals:
        if a <= present <= b:
            return name
    return "?"


def collect(root, base):
    root = Path(root)
    runs = {}
    for d in sorted(p for p in root.iterdir() if p.is_dir() and p.name.startswith("look") and (p / "shots").is_dir()):
        shots = {int(SHOT.search(f.name).group(1)): f for f in (d / "shots").glob("shot_*.png") if SHOT.search(f.name)}
        if shots:
            log = d / "log.txt"
            run = A.Run.parse(d.name, log.read_text(errors="replace")) if log.exists() else None
            runs[d.name] = {"shots": shots, "run": run, "dir": d}
    order = [base] if base in runs else []
    order += [r for r in runs if r != base]
    return runs, order


def page(root, runs, order, base):
    root = Path(root)
    presents = sorted({p for r in runs.values() for p in r["shots"]})
    plan = {r["id"]: r for r in A.load_plan().get("runs", [])}
    rows = []
    for present in presents:
        cells = []
        for rid in order:
            r = runs[rid]
            f = r["shots"].get(present)
            scene = scene_at(r["run"], present) if r["run"] else "?"
            if f is None:
                cells.append("<td class=none>no picture<br><small>%s</small></td>" % html.escape(scene))
            else:
                cells.append('<td><img loading=lazy src="%s" alt="%s present %d"><br><small>%s</small></td>' % (
                    html.escape(f.relative_to(root).as_posix()), html.escape(rid), present, html.escape(scene)))
        rows.append("<tr><th>%d</th>%s</tr>" % (present, "".join(cells)))
    heads = "".join("<th>%s<br><small>%s</small></th>" % (html.escape(rid), html.escape(plan.get(rid, {}).get("title", ""))) for rid in order)
    return ("<!doctype html><meta charset=utf-8><title>Look runs</title>"
            "<style>body{font:14px system-ui,sans-serif;margin:16px}table{border-collapse:collapse}td,th{border:1px solid #8884;padding:4px;vertical-align:top;text-align:center}"
            "img{width:320px;height:auto;display:block}small{opacity:.7}.none{opacity:.5}</style>"
            "<h1>Look runs: the same present in every run (30 fps)</h1><p>First column is the base run (%s). Present numbers are the rows.</p>"
            "<table><tr><th>present</th>%s</tr>%s</table>" % (html.escape(base), heads, "\n".join(rows)))


# ---- the core session ---------------------------------------------------------------------------------------------------------------------
CORE_VERSIONS = (("v11", "1.1"), ("cur", "1.3 current"), ("prop", "1.3 proposed"))


def png_bytes(w, h, rgb):
    """A PNG (8-bit RGB) from raw rows, standard library only."""
    raw = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    chunk = lambda kind, data: struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")


def ppm_to_png(path):
    """The Settings screen's pictures are binary PPM (P6); browsers do not show them. Writes <name>.png next to it, returns its path or None."""
    data = path.read_bytes()
    parts, at = [], 0
    while len(parts) < 4:                     # magic, width, height, maxval, separated by whitespace (comments are not written by the game)
        while at < len(data) and data[at:at + 1].isspace():
            at += 1
        start = at
        while at < len(data) and not data[at:at + 1].isspace():
            at += 1
        parts.append(data[start:at])
    if parts[0] != b"P6":
        return None
    w, h = int(parts[1]), int(parts[2])
    rgb = data[at + 1:at + 1 + w * h * 3]
    if len(rgb) != w * h * 3:
        return None
    out = path.with_suffix(".png")
    out.write_bytes(png_bytes(w, h, rgb))
    return out


def core_page(root):
    """One section per look kind (look, lookpipe, lookmega, lookrep, looksmall, lookes), one column per version, one row per present; then the Settings pages."""
    root = Path(root)
    plan = {r["id"]: r for r in A.load_plan().get("runs", [])}
    kinds = {}
    for d in sorted(p for p in root.iterdir() if p.is_dir() and (p / "shots").is_dir()):
        prefix, _, kind = d.name.partition("-")
        if prefix in dict(CORE_VERSIONS) and kind.startswith("look"):
            kinds.setdefault(kind, {})[prefix] = d
    sections = []
    for kind in sorted(kinds, key=lambda k: (len(k), k)):
        runs = {}
        for prefix, d in kinds[kind].items():
            shots = {int(SHOT.search(f.name).group(1)): f for f in (d / "shots").glob("shot_*.png") if SHOT.search(f.name)}
            log = d / "log.txt"
            runs[prefix] = (shots, A.Run.parse(d.name, log.read_text(errors="replace")) if log.exists() else None, d.name)
        cols = [(p, label) for p, label in CORE_VERSIONS if p in runs]
        presents = sorted({n for s, _, _ in runs.values() for n in s})
        rows = []
        for n in presents:
            cells = []
            for prefix, _ in cols:
                shots, run, rid = runs[prefix]
                scene = scene_at(run, n) if run else "?"
                f = shots.get(n)
                cells.append('<td><img loading=lazy src="%s" alt="%s %d"><br><small>%s</small></td>' % (html.escape(f.relative_to(root).as_posix()), html.escape(rid), n, html.escape(scene))
                             if f else "<td class=none>no picture<br><small>%s</small></td>" % html.escape(scene))
            rows.append("<tr><th>%d</th>%s</tr>" % (n, "".join(cells)))
        title = plan.get(runs[cols[0][0]][2], {}).get("title", kind) if cols else kind
        heads = "".join("<th>%s<br><small>%s</small></th>" % (html.escape(label), html.escape(runs[p][2])) for p, label in cols)
        sections.append("<h2>%s</h2><p><small>%s</small></p><table><tr><th>present</th>%s</tr>%s</table>" % (html.escape(kind), html.escape(title), heads, "\n".join(rows)))
    settings = []
    for d in sorted(p for p in root.iterdir() if p.is_dir() and (p / "shots").is_dir()):
        for f in sorted((d / "shots").glob("settings-*.ppm")):
            png = ppm_to_png(f)
            if png:
                settings.append('<figure><img src="%s" alt="%s"><figcaption>%s</figcaption></figure>' % (html.escape(png.relative_to(root).as_posix()), html.escape(f.stem), html.escape(d.name + ": " + f.stem)))
    if settings:
        sections.append("<h2>Settings screen</h2><p>Spanish: accented letters (á é í ó ú ñ ¿ ¡) must draw, not boxes or gaps.</p>" + "".join(settings))
    return ("<!doctype html><meta charset=utf-8><title>Core looks</title>"
            "<style>body{font:14px system-ui,sans-serif;margin:16px}table{border-collapse:collapse}td,th{border:1px solid #8884;padding:4px;vertical-align:top;text-align:center}"
            "img{width:320px;height:auto;display:block}small{opacity:.7}.none{opacity:.5}figure{display:inline-block;margin:4px}</style>"
            "<h1>Core session: the same moment in 1.1, 1.3 current and 1.3 proposed</h1>"
            "<p>Look for: static or noise on shadows and decals; a 1-px dark line at the right or bottom edge of glow and shadows (looksmall); glow too strong or missing; "
            "missing or wrong geometry; the mist of Pipeline Central (lookpipe). The look runs are held at 30 fps, so a row is one moment of the route "
            "(1.1 at Pipeline Central may fall below 30: its rows are the same scene, not the same instant).</p>" + "".join(sections)), len(sections)


def night_page(root, decisions=()):
    """looks-night.html: the overnight session's pictures. First the three modes (1.1 | 1.3 | LOCKED 60) of each look kind, then one section per picture switch: its baseline
    column against the switch's, with the verdict and effect of the run it belongs to."""
    root = Path(root)
    plan = {r["id"]: r for r in A.load_plan().get("runs", [])}
    verdict = {x["meta"].get("look"): x for x in decisions if x.get("meta", {}).get("look")}
    def shots_of(d):
        return {int(SHOT.search(f.name).group(1)): f for f in (d / "shots").glob("shot_*.png") if SHOT.search(f.name)} if (d / "shots").is_dir() else {}
    def run_of(d):
        log = d / "log.txt"
        return A.Run.parse(d.name, log.read_text(errors="replace")) if log.exists() else None
    def table(cols):
        """cols: [(label, run id)]"""
        loaded = [(label, rid, shots_of(root / rid), run_of(root / rid)) for label, rid in cols if (root / rid / "shots").is_dir()]
        if not loaded:
            return ""
        presents = sorted({n for _, _, s, _ in loaded for n in s})
        rows = []
        for n in presents:
            cells = []
            for label, rid, shots, run in loaded:
                f = shots.get(n)
                scene = scene_at(run, n) if run else "?"
                cells.append('<td><img loading=lazy src="%s" alt="%s %d"><br><small>%s</small></td>' % (html.escape(f.relative_to(root).as_posix()), html.escape(rid), n, html.escape(scene)) if f
                             else "<td class=none>no picture<br><small>%s</small></td>" % html.escape(scene))
            rows.append("<tr><th>%d</th>%s</tr>" % (n, "".join(cells)))
        heads = "".join("<th>%s<br><small>%s</small></th>" % (html.escape(label), html.escape(rid)) for label, rid, _, _ in loaded)
        return "<table><tr><th>present</th>%s</tr>%s</table>" % (heads, "\n".join(rows))
    sections, n_sections = [], 0
    for kind, base in (("look", "b-look"), ("lookpipe", "b-lookpipe"), ("lookmega", "b-lookmega"), ("lookrep", "b-lookrep")):
        t = table([("1.1", "o-" + kind), ("1.3 defaults", base), ("LOCKED 60", "l-" + kind)])
        if t:
            sections.append("<h2>The three modes: %s</h2>%s" % (kind, t))
            n_sections += 1
    base_of = {"look": "b-look", "cpipe": "b-lookpipe", "mega": "b-lookmega", "crep": "b-lookrep"}
    for rid, r in plan.items():
        if r.get("role") != "nlook" or r.get("night", {}).get("base") or not rid.startswith("n-look") or not (root / rid / "shots").is_dir():
            continue
        t = table([("1.3 defaults", base_of.get(r["route"], "b-look")), (rid[2:], rid)])
        x = verdict.get(rid)
        note = "<p><b>%s</b>: %s<br><small>%s</small></p>" % (html.escape(x["id"]), html.escape(x["verdict"]), html.escape(x["effect"])) if x else "<p><small>%s</small></p>" % html.escape(r.get("title", rid))
        sections.append("<h2>%s</h2>%s%s" % (html.escape(rid), note, t))
        n_sections += 1
    return ("<!doctype html><meta charset=utf-8><title>The night's pictures</title>"
            "<style>body{font:14px system-ui,sans-serif;margin:16px}table{border-collapse:collapse}td,th{border:1px solid #8884;padding:4px;vertical-align:top;text-align:center}"
            "img{width:320px;height:auto;display:block}small{opacity:.7}.none{opacity:.5}</style>"
            "<h1>The night's pictures: the same moment in every column (30 fps)</h1>"
            "<p>Look for: static or noise on shadows and decals; a dark line at the right or bottom edge; glow too strong or missing; missing or wrong geometry; "
            "blur or shimmer on textures; the HUD's edges; Pipeline Central's mist; Mega Strike sprites.</p>" + "".join(sections)), n_sections


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("session")
    ap.add_argument("--base", default="lookbase")
    ap.add_argument("--out", default="looks.html")
    ap.add_argument("--core", action="store_true", help="the core session's page, looks-core.html")
    args = ap.parse_args()
    if args.core:
        text, n = core_page(args.session)
        out = Path(args.session) / "looks-core.html"
        out.write_text(text)
        print("wrote %s (%d sections)" % (out, n))
        return 0
    runs, order = collect(args.session, args.base)
    if not runs:
        print("no look runs with shots/ in %s" % args.session, file=sys.stderr)
        return 2
    out = Path(args.session) / args.out
    out.write_text(page(args.session, runs, order, args.base))
    print("wrote %s (%d runs, %d presents)" % (out, len(order), len({p for r in runs.values() for p in r["shots"]})))
    return 0


if __name__ == "__main__":
    sys.exit(main())
