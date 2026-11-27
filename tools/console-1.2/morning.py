"""MORNING.md: the page the overnight session writes for the owner. One page, plain words; the detail follows after the line.

    python3 issues.py <session dir> --morning [--pixels]       (issues.py calls this module)

What it says, in this order
  1. the night in one paragraph: how many runs finished, crashed, were skipped, how long it took
  2. what crashed (stability: the twelve-match route, the stadium intros, the natural match ends, 1.1 against 1.3, dumps, the C heap)
  3. frames per second for every scene class: 1.1, 1.3 as it is, LOCKED 60
  4. which switches earned their place (each judged against the same-night baseline of the scene it acts on, with the noise rule of night.py)
  5. the picture trade-offs: for each, the file names of the screenshots to open (and with --pixels, which pictures changed most)
  6. the proposed default set, as lines for the settings file
  7. what was not run and why
Standard library only; every number comes from the console logs.
"""
import csv
import html
import re
import statistics
from collections import OrderedDict, defaultdict
from pathlib import Path

import analyse as A

SCENE_NAMES = OrderedDict([
    ("menu-main", "Main menu"), ("stadium-select", "Stadium select"), ("walkout", "Walk-out (intro)"), ("play", "Open play"), ("goal:nis", "Goal celebration"),
    ("goal:replay", "Goal replay"), ("pipe-walkout", "Pipeline Central intro"), ("pipe-play", "Pipeline Central play"), ("pipe:nis", "Pipeline Central celebration"),
    ("pipe:replay", "Pipeline Central replay"), ("lava-walkout", "Lava Pit intro"), ("lava-play", "Lava Pit play (items)"), ("lava:nis", "Lava Pit celebration"),
    ("lava:replay", "Lava Pit replay"), ("mega-save", "Mega Strike save"), ("rep:nis", "Celebrations (goal route)"), ("rep:replay", "Replays (goal route)")])
HOLDS_60 = (58.0, 2.0)      # a locked-60 scene "holds" at this fps with at most this many percent of late pictures
NEAR_60 = 55.0
LATE_WORSE_PT = 1.5          # percentage points of late pictures that make a switch worse
SETTINGS = {}


def scene_name(cls):
    return SCENE_NAMES.get(cls, cls)


def _med(values):
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else None


def _mean(values):
    values = [v for v in values if v is not None]
    return sum(values) / len(values) if values else None


def _fmt(v, f="%.1f", dash="-"):
    return f % v if v is not None else dash


# ---- the data -----------------------------------------------------------------------------------------------------------------------------------------

def load(root):
    plan = A.load_plan()
    night = plan.get("night", {})
    data = A.night_stats(root, warm=night.get("warm_records", 2))
    root = Path(root)
    status, dumps = {}, {}
    for rid in data:
        d = root / rid
        status[rid] = (d / "status.txt").read_text().strip() if (d / "status.txt").exists() else ""
        dumps[rid] = (d / "dumps.txt").read_text().split() if (d / "dumps.txt").exists() else []
    progress = []
    p = root / "night-progress.txt"
    if p.exists():
        with open(p, newline="") as f:
            progress = list(csv.DictReader(f))
    skipped = []
    p = root / "skipped.txt"
    if p.exists():
        skipped = [l.split(",") for l in p.read_text().splitlines() if l.strip()]
    return plan, night, data, status, dumps, progress, skipped


def ids_of(data, role):
    return [rid for rid, d in data.items() if d["role"] == role]


def group(data, role, cls, key):
    vals = [d["stats"][cls][key] for rid, d in data.items() if d["role"] == role and cls in d["stats"]]
    return _mean(vals), len(vals)


def extras_of(data, role, cls, key, sub=None):
    vals = []
    for rid, d in data.items():
        if d["role"] != role or cls not in d["extras"]:
            continue
        e = d["extras"][cls]
        vals.append(e[key].get(sub) if sub else e.get(key))
    return _med(vals)


def size_of(data, role, cls):
    sizes = [d["extras"][cls]["size"] for d in data.values() if d["role"] == role and cls in d["extras"] and d["extras"][cls].get("size")]
    return max(set(sizes), key=sizes.count) if sizes else None


def ended_ok(d, st):
    return d["run"].done and st in ("done", "") and not any(re.search(r"FATAL|CRASH|stall: no frame|\[fatal ", p) for p in d["run"].problems)


# ---- stability ----------------------------------------------------------------------------------------------------------------------------------------

def stability(data, status, dumps):
    """-> (list of problem dicts, list of summary lines)"""
    problems, lines = [], []
    for rid, d in data.items():
        run, st = d["run"], status.get(rid, "")
        why = []
        if st and st != "done":
            why.append("did not finish (%s)" % st)
        if dumps.get(rid):
            why.append("crash dump %s" % ", ".join(dumps[rid]))
        fatal = [p for p in run.problems if re.search(r"FATAL|CRASH|stall: no frame|\[fatal ", p)]
        if fatal:
            why.append("fatal line: %s" % fatal[0][:90])
        s = A.stability(run)
        if s["heap_fill"] is not None and s["heap_fill"] >= A.HEAP_WARN:
            why.append("C heap at %.0f%% of its block" % (100 * s["heap_fill"]))
        if s["play_compile_records"] and d["version"] == "1.3" and not d["meta"].get("pack"):
            why.append("%d record(s) in play with a shader compile" % s["play_compile_records"])
        if why:
            last = [m for m in run.marks][-1][1] if run.marks else "before the first scene mark"
            problems.append({"run": rid, "version": d["version"], "route": d["route"], "why": "; ".join(why), "last_mark": last, "stab": d["role"] == "nstab"})
    for rid, d in data.items():
        if d["role"] != "nstab":
            continue
        want = d["meta"].get("expect", 0)
        marks = [t for _, t in d["run"].marks]
        reached = len({t.split()[1] for t in marks if t.startswith("@end ") and re.match(r"@end (stab|end)-", t)})
        begun = len({t.split()[1] for t in marks if t.startswith("@begin ") and re.match(r"@begin (stab|end)-", t)})
        series = d["run"].heap_series
        trend = ""
        if len(series) >= 9:
            third = len(series) // 3
            first, last = _mean(series[:third]), _mean(series[-third:])
            trend = ", C heap in use %.0f -> %.0f MiB (first third -> last third)%s" % (first / 1024, last / 1024, " CLIMBING" if last - first > 8 * 1024 else "")
        h = d["run"].heap
        lines.append("%s (%s): %d of %d matches ended, %d begun, %s%s; heap peak %.0f MiB of %.0f" % (
            rid, d["version"], reached, want, begun, "route ended" if d["run"].done else "route did NOT end", trend, h["peak"] / 1024.0, (h["block"] or 0) / 1024.0))
    return problems, lines


# ---- the scene table ----------------------------------------------------------------------------------------------------------------------------------

def scene_rows(data):
    classes = sorted({c for d in data.values() if d["role"] in ("nref", "n60", "n11") for c in d["stats"]}, key=A.class_key)
    rows = []
    for cls in classes:
        row = {"cls": cls}
        for tag, role in (("o", "n11"), ("b", "nref"), ("l", "n60")):
            fps, n = group(data, role, cls, "fps")
            if fps is None:
                row[tag] = None
                continue
            row[tag] = {"n": n, "fps": fps, "mean": group(data, role, cls, "mean")[0], "p95": group(data, role, cls, "p95")[0], "late": extras_of(data, role, cls, "late"),
                        "size": size_of(data, role, cls), "gpu": extras_of(data, role, cls, "gpu_busy"), "scenes": extras_of(data, role, cls, "scenes"),
                        "lane": {k: extras_of(data, role, cls, "lane", k) for k in ("game", "send", "fe", "be", "gpu", "dc")}, "hitches": sum(
                            d["extras"][cls].get("hitches", 0) for d in data.values() if d["role"] == role and cls in d["extras"])}
        b = A.baseline(data, ids_of(data, "nref"), cls, "fps")
        row["noise"], row["nb"] = (b[1], b[2]) if b else (None, 0)
        rows.append(row)
    return rows


def lock60_line(cell):
    if not cell:
        return "-"
    held = cell["fps"] >= HOLDS_60[0] and (cell["late"] is None or cell["late"] <= HOLDS_60[1])
    return "holds 60" if held else ("close" if cell["fps"] >= NEAR_60 else "short of 60")


# ---- decisions ----------------------------------------------------------------------------------------------------------------------------------------

def refs_for(meta, night):
    ref = meta.get("ref", "b")
    if ref in ("b", "l", "o"):
        return night["baseline_refs"][ref]
    return [ref]


def noise_for(data, refs, night, cls, key):
    """(mean, noise, n, guessed) of the baseline of one scene. A baseline of one run (the LOCKED 60 set before its second run exists) borrows the 1.3 defaults' noise."""
    b = A.baseline(data, refs, cls, key)
    if b is None:
        return None
    mean, noise, n = b
    guessed = n < 2
    if guessed:
        bb = A.baseline(data, night["baseline_refs"]["b"], cls, key)
        if bb and bb[2] >= 2:
            noise = max(noise, bb[1])
    return mean, noise, n, guessed


def classes_of(meta, d):
    tgt = [c for c in (meta.get("target") or []) if c in d["stats"]]
    return tgt or list(d["stats"])


def decide(rid, data, night, status, dumps):
    """The verdict of one A/B run: dict(verdict, short, effect, marginal, scenes {cls: (dfps, noise, dp95, noise95)}, proposed (env or None), why)."""
    d = data[rid]
    meta = d["meta"]
    on, kind = meta.get("on", True), meta.get("kind", "speed")
    out = {"id": rid, "kind": kind, "on": on, "env": d["env"], "meta": meta}
    st = status.get(rid, "")
    run = d["run"]
    if (st and st != "done") or dumps.get(rid) or not run.done:
        out.update(verdict="BROKEN: the run did not end (%s)" % (st or "no end"), short="broken", effect="-", marginal=False, scenes={})
        return out
    refs = refs_for(meta, night)
    scenes, better, worse, notes = {}, [], [], []
    for cls in classes_of(meta, d):
        nb = noise_for(data, refs, night, cls, "fps")
        if not nb:
            continue
        fps_mean, fps_noise, n, guessed = nb
        s = d["stats"][cls]
        dfps = s["fps"] - fps_mean
        pb = noise_for(data, refs, night, cls, "p95")
        dp95, p95_noise = (s["p95"] - pb[0], pb[1]) if pb else (0.0, 99.0)
        late = d["extras"].get(cls, {}).get("late")
        late_base = _med([data[r]["extras"][cls].get("late") for r in refs if r in data and cls in data[r]["extras"]])
        dlate = (late - late_base) if late is not None and late_base is not None else 0.0
        scenes[cls] = (dfps, fps_noise, dp95, p95_noise, guessed, dlate)
        worth = max(fps_noise, night.get("worth_fps", 0.5))
        is_better = dfps >= worth or (dp95 <= -p95_noise and dfps > -fps_noise)
        is_worse = dfps <= -fps_noise or (dp95 >= p95_noise and dfps < fps_noise) or dlate >= LATE_WORSE_PT
        if is_worse:
            worse.append(cls)
        elif is_better:
            better.append(cls)
    # the log must show the switch acted when it says how
    proof = meta.get("proof")
    acted = True
    if proof:
        acted = any(re.search(proof, l) for l in d.get("lines", []))
        if not acted:
            notes.append("NO LOG LINE shows the switch acted (%s): the build may not have it" % proof)
    s_stab = A.stability(run)
    if s_stab["play_compile_records"] and not meta.get("pack"):
        notes.append("%d play record(s) held a shader compile" % s_stab["play_compile_records"])
    if s_stab["heap_fill"] is not None and s_stab["heap_fill"] >= A.HEAP_WARN:
        notes.append("C heap at %.0f%%" % (100 * s_stab["heap_fill"]))
    effects = []
    for cls, (dfps, nz, dp95, nz95, guessed, dlate) in scenes.items():
        effects.append("%s %+.1f fps (noise %.1f%s), p95 %+.1f ms%s" % (scene_name(cls), dfps, nz, " guessed" if guessed else "", dp95, (", late %+.1f pt" % dlate) if abs(dlate) >= 0.5 else ""))
    biggest = max((abs(v[0]) for v in scenes.values()), default=0.0)
    thr = max((v[1] for v in scenes.values()), default=0.3)
    out["marginal"] = bool(scenes) and biggest < 2 * max(thr, night.get("worth_fps", 0.5)) and bool(better or worse)
    out["scenes"] = scenes
    out["effect"] = "; ".join(effects) or "no usable window in the target scene"
    out["notes"] = notes
    verdict, short, proposed = "", "same", None
    if not scenes:
        verdict, short = "NO DATA (the target scene had no usable window)", "nodata"
    elif not acted:
        verdict, short = "INCONCLUSIVE (the switch left no trace in the log)", "inconclusive"
    elif kind == "info":
        verdict, short = "INFO (judged by its lines, hitches and compiles; frame time: %s)" % ("better" if better else "worse" if worse else "same"), "info"
    elif on:       # the run turned something ON: is it worth making the default?
        if worse:
            verdict, short = "LEAVE OFF (slower in %s)" % ", ".join(scene_name(c) for c in worse), "leave-off"
        elif better:
            if kind == "picture":
                verdict, short = "ADOPT IF THE PICTURES AGREE (faster in %s)" % ", ".join(scene_name(c) for c in better), "adopt-look"
            elif kind == "set":
                verdict, short = "THE SET HOLDS (faster in %s, slower nowhere)" % ", ".join(scene_name(c) for c in better), "set-ok"
            else:
                verdict, short = "ADOPT (faster in %s)" % ", ".join(scene_name(c) for c in better), "adopt"
            proposed = dict(d["env"])
        else:
            verdict, short = ("THE SET CHANGES NOTHING MEASURABLE" if kind == "set" else "NO GAIN: leave off"), "no-gain"
    else:          # the run turned a default OFF: does the default earn its place?
        if worse:
            verdict, short = "KEEP ON: it earns its place (off is slower in %s)" % ", ".join(scene_name(c) for c in worse), "keep"
        elif better:
            verdict, short = "FLIP OFF: it costs more than it gives (off is faster in %s)" % ", ".join(scene_name(c) for c in better), "flip"
            proposed = dict(d["env"])
        else:
            verdict, short = ("NO MEASURABLE EFFECT: keep (a picture feature)" if kind == "picture" else "NO MEASURABLE EFFECT: could be hidden from Settings"), "no-effect"
    if kind == "set" and worse:
        verdict, short = "THE SET HURTS (slower in %s)" % ", ".join(scene_name(c) for c in worse), "set-bad"
    if out["marginal"] and short not in ("broken", "nodata", "inconclusive", "info"):
        verdict += " [marginal: rerun it]"
    if notes:
        verdict += " (" + "; ".join(notes) + ")"
    out.update(verdict=verdict, short=short, proposed=proposed, better=better, worse=worse)
    return out


def attach_lines(root, data):
    for rid, d in data.items():
        log = Path(root) / rid / "log.txt"
        d["lines"] = log.read_text(errors="replace").splitlines() if log.exists() else []


# ---- pictures -----------------------------------------------------------------------------------------------------------------------------------------

def shots_of(root, rid):
    d = Path(root) / rid / "shots"
    return sorted(d.glob("shot_*.png")) if d.is_dir() else []


def pixel_diff(a_path, b_path, decode):
    """(share of pixels that differ by more than 2 levels in any channel, mean level difference) between two screenshots of the same size, else None."""
    a, b = decode(a_path.read_bytes()), decode(b_path.read_bytes())
    if not a or not b or a[:2] != b[:2]:
        return None
    w, h, ra = a
    rb = b[2]
    diff = total = 0
    for x, y in zip(ra, rb):
        if x == y:
            continue
        for i in range(0, len(x), 3):
            d = max(abs(x[i] - y[i]), abs(x[i + 1] - y[i + 1]), abs(x[i + 2] - y[i + 2]))
            if d > 2:
                diff += 1
                total += d
    return diff / float(w * h), (total / diff if diff else 0.0)


LOOK_BASE = {"look": "b-look", "cpipe": "b-lookpipe", "mega": "b-lookmega", "crep": "b-lookrep"}


def picture_section(root, decisions, plan_runs, pixels, decode):
    """[(switch id, verdict short, look run, baseline look run, file names, auto text)] for every decision with a look run."""
    out = []
    for dec in decisions:
        look = dec["meta"].get("look")
        if not look or look not in plan_runs:
            continue
        base = LOOK_BASE.get(plan_runs[look]["route"])
        files = shots_of(root, look)
        entry = {"switch": dec["id"], "verdict": dec["verdict"], "look": look, "base": base, "files": [f.name for f in files], "auto": [], "changed": []}
        if pixels and base and files:
            bases = {f.name: f for f in shots_of(root, base)}
            pick = [f for f in files if f.name in bases]
            step = max(1, len(pick) // 5)
            scored = []
            for f in pick[::step][:6]:
                r = pixel_diff(bases[f.name], f, decode)
                if r:
                    scored.append((r[0], r[1], f.name))
            scored.sort(reverse=True)
            entry["changed"] = ["%s: %.1f%% of the pixels differ, by %.0f levels on average" % (n, 100 * share, lv) for share, lv, n in scored[:3]]
        out.append(entry)
    return out


# ---- the page -----------------------------------------------------------------------------------------------------------------------------------------

def settings_row(env, rows_of):
    names = sorted({r for k in env for r in rows_of.get(k, [])})
    return ", ".join(names) or "no Settings row"


def env_text(env):
    return " ".join("%s=%s" % kv for kv in env.items()) if isinstance(env, dict) else " ".join("%s=%s" % kv for kv in env)


def write(root, out_path, rows_of=None, pixels=False, decode=None):
    root = Path(root)
    rows_of = rows_of or {}
    plan, night, data, status, dumps, progress, skipped = load(root)
    if not data:
        Path(out_path).write_text("# Strikers Charged Vita 1.3: the night\n\nNo night runs in this folder yet.\n")
        return {}
    attach_lines(root, data)
    plan_runs = {r["id"]: r for r in plan.get("runs", [])}
    problems, stab_lines = stability(data, status, dumps)
    rows = scene_rows(data)
    decisions = [decide(rid, data, night, status, dumps) for rid, d in data.items() if d["role"] == "nab"]
    pics = picture_section(root, [x for x in decisions if x["kind"] == "picture"], plan_runs, pixels, decode)
    finished = [rid for rid in data if status.get(rid) == "done"]
    total_scheduled = len([r for r in plan.get("runs", []) if r.get("night") and r["night"].get("phase") not in ("p5",)])
    elapsed = progress[-1]["elapsed_min"] if progress else "?"
    o = []
    w = lambda s="": o.append(s)
    w("# Strikers Charged Vita 1.3: the night on the real Vita")
    w()
    broke = len({p["run"] for p in problems})
    w("**%d runs have a log, %d ended cleanly, %d had a problem, %d were skipped; the clock says %s minutes.** %s" % (
        len(data), len(finished), broke, len(skipped), elapsed,
        "Nothing crashed." if not any("did not finish" in p["why"] or "crash dump" in p["why"] or "fatal" in p["why"] for p in problems) else "Something crashed: see below."))
    w()
    # ---- 1 crashes
    w("## 1. What crashed")
    w()
    hard = [p for p in problems if re.search(r"did not finish|crash dump|fatal", p["why"])]
    if hard:
        for p in hard:
            w("- **%s** (%s, route %s): %s. Last step before it: `%s`. Logs: `%s/log.txt`%s." % (
                p["run"], p["version"], p["route"], p["why"], p["last_mark"], p["run"], (", dumps in `%s/dumps/`" % p["run"]) if dumps.get(p["run"]) else ""))
    else:
        w("Nothing: no run stopped early, no crash dump, no fatal line.")
    soft = [p for p in problems if p not in hard]
    for p in soft:
        w("- %s (%s): %s." % (p["run"], p["version"], p["why"]))
    w()
    for line in stab_lines:
        w("- " + line)
    w("- Sleep and resume were not tried: nothing can script them. Do it by hand once (close the lid, wake it, play a minute).")
    w()
    # ---- 2 fps
    w("## 2. Frames per second, scene by scene")
    w()
    n_b = max((r["nb"] for r in rows), default=0)
    w("Median of the steady 300-picture windows after a 600-picture warm-up. fps / p95 frame time in ms / late pictures. The 1.3 numbers are the mean of %d runs spread over the night; "
      "**same-night noise** is shown per scene (2 x the spread of those runs): a difference smaller than it is not a difference." % n_b)
    w()
    w("| scene | 1.1 | 1.3 as it is | LOCKED 60 | noise (fps) | 1.3 vs 1.1 | LOCKED 60 |")
    w("| --- | --- | --- | --- | --- | --- | --- |")
    cellf = lambda c: ("%.1f fps / %.1f ms / %s%% late" % (c["fps"], c["p95"], _fmt(c["late"]))) + (" @%s" % A.fmt_size(c["size"]) if c["size"] else "") if c else "-"
    for r in rows:
        o_, b_, l_ = r["o"], r["b"], r["l"]
        vs11 = "-"
        if o_ and b_:
            d = b_["fps"] - o_["fps"]
            lim = r["noise"] or 0.5
            vs11 = "%+.1f fps %s" % (d, "better" if d > lim else "WORSE" if d < -lim else "same")
        w("| %s | %s | %s | %s | %s | %s | %s |" % (scene_name(r["cls"]), cellf(o_), cellf(b_), cellf(l_), _fmt(r["noise"], "%.1f"), vs11, lock60_line(l_)))
    w()
    # ---- 3 switches
    w("## 3. Which switches earned their place")
    w()
    groups = OrderedDict([("Earned a place: turn it on, or keep it on", ("adopt", "keep", "set-ok")), ("Costs more than it gives, or does nothing: leave it off, or flip it", ("flip", "leave-off", "no-gain", "no-effect", "set-bad")),
                          ("Picture switches that are faster: your eyes decide (section 4)", ("adopt-look",)), ("No verdict: lines to read, or no data", ("info", "inconclusive", "nodata", "broken"))])
    for title, keys in groups.items():
        items = [x for x in decisions if x["short"] in keys]
        if not items:
            continue
        w("**%s** (%d)" % (title, len(items)))
        w()
        w("| switch (run) | what the run changed | verdict | effect on the scene it acts on | Settings row |")
        w("| --- | --- | --- | --- | --- |")
        for x in items:
            w("| %s | %s%s | %s | %s | %s |" % (x["id"], "turns on " if x["on"] else "turns off ", "`%s`" % env_text(_diff_env(x["env"])), x["verdict"], x["effect"], settings_row(x["env"], rows_of)))
        w()
    # ---- 4 pictures
    w("## 4. Picture trade-offs: open these files")
    w()
    w("Each picture switch has a look run at a locked 30 fps (same match moment in every run). Open the same file name in the two folders. `looks-night.html` shows them side by side.")
    w()
    if pics:
        w("| switch | verdict | baseline pictures | switch pictures | most changed (automatic) |")
        w("| --- | --- | --- | --- | --- |")
        for e in pics:
            names = e["files"]
            sample = "`%s/shots/%s` ... %d pictures" % (e["look"], names[0], len(names)) if names else "none came back"
            w("| %s | %s | `%s/shots/` | %s | %s |" % (e["switch"], e["verdict"].split(" (")[0], e["base"], sample, "; ".join(e["changed"]) or ("(run with --pixels)" if not pixels else "-")))
    else:
        w("No picture look runs finished.")
    w()
    w("The pictures of the three modes: `o-look*/shots` (1.1), `b-look*/shots` (1.3), `l-look*/shots` (LOCKED 60).")
    w()
    # ---- 5 proposed set
    w("## 5. The proposed default set")
    w()
    adopt = [x for x in decisions if x["short"] in ("adopt", "flip") and x["proposed"]]
    maybe = [x for x in decisions if x["short"] == "adopt-look" and x["proposed"]]
    sets = [x for x in decisions if x["kind"] == "set"]
    lock = [r for r in rows if r["l"]]
    held = [r for r in lock if lock60_line(r["l"]) == "holds 60"]
    w("**LOCKED 60** holds 60 in %d of %d scenes it ran in%s." % (len(held), len(lock), (" (short in: %s)" % ", ".join(scene_name(r["cls"]) for r in lock if lock60_line(r["l"]) != "holds 60")) if len(held) < len(lock) else ""))
    for x in sets:
        w("- %s: %s" % (x["id"], x["verdict"]))
    w()
    w("Switches to set (speed only, no picture change beyond the owner's tolerance), as lines of the settings file:")
    w()
    w("```")
    seen = {}
    for x in adopt:
        for k, v in x["proposed"].items():
            seen.setdefault(k, (v, x["id"]))
    for k, (v, rid) in seen.items():
        w("%s=%s   # %s" % (k, v, rid))
    if not seen:
        w("# nothing earned a change tonight")
    w("```")
    if maybe:
        w()
        w("Faster but with a picture cost, to decide with section 4: " + ", ".join("%s (`%s`)" % (x["id"], env_text(_diff_env(x["env"]))) for x in maybe) + ".")
    w()
    w("Rule used: the effect in the scene the switch acts on is beyond the same-night noise and beyond %.1f fps (or p95 is lower beyond its noise with fps no worse), nothing got slower beyond the noise, "
      "late pictures did not rise by %.1f points, nothing crashed, and for a picture switch the pictures look right. `[marginal]` marks an effect under twice the threshold: rerun it before trusting it." % (night.get("worth_fps", 0.5), LATE_WORSE_PT))
    w()
    # ---- 6 not run
    w("## 6. Not run")
    w()
    if skipped:
        by = defaultdict(list)
        for row in skipped:
            by[row[2] if len(row) > 2 else "?"].append(row[0])
        for why, ids in by.items():
            w("- %s: %s" % ({"deadline": "past its phase's deadline", "night-end": "would have ended after the night", "swap-failed": "its version could not be put on the console"}.get(why, why), ", ".join(ids)))
    p = root / "not-in-this-build.txt"
    if p.exists() and p.read_text().strip():
        w("- the build does not have these switches yet (their branch is not merged): " + ", ".join(l.split("\t")[0] for l in p.read_text().splitlines() if l.strip()))
    if not skipped and not (p.exists() and p.read_text().strip()):
        w("- everything in the plan ran")
    w()
    # ---- detail
    w("---")
    w()
    w("## Detail for the engineer")
    w()
    w("| scene | config | runs | fps | mean ms | p95 ms | late % | size | GPU ms | scenes | draws | game/send/fe/be ms | hitches |")
    w("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    for r in rows:
        for tag, label in (("o", "1.1"), ("b", "1.3"), ("l", "LOCKED 60")):
            c = r[tag]
            if not c:
                continue
            ln = c["lane"]
            w("| %s | %s | %d | %.1f | %s | %.1f | %s | %s | %s | %s | %s | %s | %d |" % (scene_name(r["cls"]), label, c["n"], c["fps"], _fmt(c["mean"], "%.1f"), c["p95"], _fmt(c["late"]), A.fmt_size(c["size"]), _fmt(c["gpu"]),
                                                                                    _fmt(c["scenes"]), _fmt(ln.get("dc"), "%.0f"), "/".join(_fmt(ln.get(k)) for k in ("game", "send", "fe", "be")), c["hitches"]))
    w()
    l60 = []
    for rid, d in data.items():
        if d["role"] == "n60" and d["run"].lock60:
            for scene, meas, over, pct, down, up in d["run"].lock60:
                l60.append("%s: %s %d frames, %.1f%% over the 60 budget, %d steps down, %d up" % (rid, scene, meas, pct, down, up))
    if l60:
        w("LOCKED 60 governor lines:")
        w()
        for l in l60:
            w("- " + l)
        w()
    w("Heap by run: " + "; ".join("%s %.0f/%.0f MiB" % (rid, d["run"].heap["peak"] / 1024.0, (d["run"].heap["block"] or 0) / 1024.0) for rid, d in data.items() if d["run"].heap["block"]))
    Path(out_path).write_text("\n".join(o) + "\n")
    return {"decisions": decisions, "rows": rows, "problems": problems, "pictures": pics, "stab": stab_lines}


def _diff_env(env):
    """The run's switches without the ones every run of its route carries (the lock, the lava camera)."""
    drop = {"MSC_CAMERA_ZOOM", "MSC_CAMERA_AUTO", "MSC_FX_STAT", "MSC_FX_TIGHT_STAT", "MSC_AMORT_LOG", "MSC_SBATCH_STATS", "MSC_VGX_SBATCH_STATS", "MSC_VITA_RT_LOG"}
    return {k: v for k, v in (env.items() if isinstance(env, dict) else env) if k not in drop}
