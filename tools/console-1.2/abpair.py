#!/usr/bin/env python3
"""abpair: compare matched scene windows of switch-ON and switch-OFF console logs.

Windows are taken from three line kinds and grouped by scene class so that
ON and OFF are compared like with like:

  [lane]   300-picture windows, classed by draw count dc:
           play 180-279, heavy >= 280, light < 180.
  [head]   30 s windows by scene class (play, cut-scene, replay, other);
           kept only with >= 300 pictures.
  [present] windows with a "smooth N/100" token (vgx_smooth.h; an estimate,
           HIGHER is better, the verdict accounts for it), class "all".
  [gsplit] 300-picture windows, classed by steps per picture:
           live > 1.4, non-live otherwise. [gsplit-phys] and [gsplit-ai]
           lines that follow a [gsplit] line supply per-step phase times.

For every metric and class the tool prints the number of windows on each
side, the mean and median of each side, the difference of means (ON minus
OFF), a bootstrap 90 % confidence interval of that difference (2000
resamples, fixed seed) and a verdict.  All metrics are times, so lower is
better: an interval entirely below zero reads "ON better", entirely above
zero "OFF better", anything else "no difference".  A class with no windows
on a side reads "no data"; one with fewer than 2 windows on a side reads
"too few windows" because a bootstrap of one value says nothing.

Usage:
  abpair.py --on a1.log [a2.log ...] --off b1.log [b2.log ...]
            [--metric fe,gpu,...] [--json] [--paired]
  abpair.py --inrun run1.log [run2.log ...] [--metric fps,fe,...] [--json]

--inrun (wave 15, helper ab15): ONE run with MSC_AB=<KEY>:<a>,<b>:<window s> (renderer/src/vgx_ab.h): the switch alternates a, b, a, b ... inside the run and every
[lane] line is cut at the flip and tagged "ab KEY=v a|b wN" ("ab-settle" and "ab-pre" lines are skipped). The b windows are compared with the a windows beside them
(b minus the mean of its neighbours a(N-1), a(N+1); at the end of a run the one neighbour there is), so heat, battery and card drift cancel inside the run; the bootstrap
is over those differences; a neighbour whose draw count dc differs by more than 15 % (--dc-tol) is another scene and is left out. "ON" is b (the candidate), "OFF" is a (the baseline). Metrics: fps (HIGHER is better) and the [lane] columns fe, be_cpu, be_wall, gpu, game.

Standard library only.
"""

import argparse
import json
import random
import re
import sys

BOOT_RESAMPLES = 2000
BOOT_SEED = 1307
LIVE_STEPS_PER_PIC = 1.4
HEAD_MIN_PICS = 300

LANE_KEYS = {"game", "send", "fe", "be", "gpu", "dc"}

LANE_RE = re.compile(r"^\[lane\] (\d+) pics\b")
HEAD_RE = re.compile(r"^\[head\] \d+ s, class (\S+) \(.*?\): (\d+) pictures")
HEAD_STEP_RE = re.compile(r"game ms: with step p50 (\S+) p95 (\S+) max")
HEAD_RENDER_RE = re.compile(r"render thread ms p95 (\S+) max")
HEAD_GPU_RE = re.compile(r"GPU ms p95 (\S+) max")
GS_RE = re.compile(
    r"^\[gsplit\] (\d+) pics wall \S+ ms/pic: sim (\S+) \(steps (\S+)/pic:[^)]*\)"
    r" render-task (\S+) \("
)
# gxvita14: the [present] line (vgx_present.cpp SyncLog) ends in "smooth 87/100 (ideal 95 %, judder 0.08, missed 0.50/s)" (vgx_smooth.h); a window is its own class, "all"
PRESENT_RE = re.compile(r"^\[present\] .*?, ([\d.]+) s, (\d+) pictures\b")
SMOOTH_RE = re.compile(r"\bsmooth (\d+)/100\b")
GS_PHYS_RE = re.compile(r"^\[gsplit-phys\] .*?ms per STEP: physics (\S+)")
GS_AI_RE = re.compile(r"^\[gsplit-ai\] .*?ms per STEP: ai (\S+)")

# metric name -> (source, label)
METRICS = [
    ("fe", "lane", "lane fe"),
    ("be_cpu", "lane", "lane be cpu"),
    ("be_wall", "lane", "lane be wall"),
    ("gpu", "lane", "lane gpu"),
    ("game", "lane", "lane game"),
    ("step_p50", "head", "head step p50"),
    ("step_p95", "head", "head step p95"),
    ("render_p95", "head", "head render p95"),
    ("gpu_p95", "head", "head gpu p95"),
    ("sim_step", "gsplit", "gsplit sim/step"),
    ("phys_step", "gsplit", "gsplit physics/step"),
    ("ai_step", "gsplit", "gsplit ai/step"),
    ("render_task", "gsplit", "gsplit render-task"),
    ("smooth", "present", "present smoothness"),   # a score 0..100: HIGHER is better (HIGHER_BETTER)
]
HIGHER_BETTER = {"smooth", "fps"}
INRUN_METRICS = ["fps", "fe", "be_cpu", "be_wall", "gpu", "game"]
INRUN_LABEL = {"fps": "lane fps (pictures/s)", "fe": "lane fe", "be_cpu": "lane be cpu", "be_wall": "lane be wall", "gpu": "lane gpu", "game": "lane game"}
LANE_AB_RE = re.compile(r"\bab (\S+?)=(\S+) ([ab]) w(\d+)\s*$")
LANE_RATE_RE = re.compile(r"^\[lane\] (\d+) pics ([\d.]+)s")
PRESENT_MIN_PICS = 100
METRIC_NAMES = [m[0] for m in METRICS]
METRIC_SOURCE = {m[0]: m[1] for m in METRICS}


def num(tok):
    """Return a non-negative float, or None for '-' and the -1.0 no-data marker."""
    try:
        value = float(tok)
    except (TypeError, ValueError):
        return None
    return value if value >= 0 else None


def lane_class(dc):
    if dc >= 280:
        return "heavy"
    if dc >= 180:
        return "play"
    return "light"


def parse_lane(line):
    m = LANE_RE.match(line)
    if not m:
        return None
    kv = {}
    toks = line.split()
    for i, tok in enumerate(toks[:-1]):
        if tok in LANE_KEYS and tok not in kv:
            kv[tok] = toks[i + 1]
    if "dc" not in kv:
        return None
    try:
        dc = int(kv["dc"])
    except ValueError:
        return None
    metrics = {"fe": num(kv.get("fe", "-")), "gpu": num(kv.get("gpu", "-")),
               "game": num(kv.get("game", "-")),
               "be_cpu": None, "be_wall": None}
    if "be" in kv and "/" in kv["be"]:
        cpu, wall = kv["be"].split("/", 1)
        metrics["be_cpu"] = num(cpu)
        metrics["be_wall"] = num(wall)
    return {"source": "lane", "cls": lane_class(dc), "pics": int(m.group(1)), "dc": dc,
            "metrics": metrics}


def parse_head(line):
    m = HEAD_RE.match(line)
    if not m:
        return None
    cls, pics = m.group(1), int(m.group(2))
    if pics < HEAD_MIN_PICS:
        return None
    step = HEAD_STEP_RE.search(line)
    render = HEAD_RENDER_RE.search(line)
    gpu = HEAD_GPU_RE.search(line)
    metrics = {
        "step_p50": num(step.group(1)) if step else None,
        "step_p95": num(step.group(2)) if step else None,
        "render_p95": num(render.group(1)) if render else None,
        "gpu_p95": num(gpu.group(1)) if gpu else None,
    }
    return {"source": "head", "cls": cls, "pics": pics, "metrics": metrics}


def parse_gsplit(line):
    m = GS_RE.match(line)
    if not m:
        return None
    pics = int(m.group(1))
    sim_pic = num(m.group(2))
    steps_pic = num(m.group(3))
    render = num(m.group(4))
    if steps_pic is None:
        steps_pic = 0.0
    cls = "live" if steps_pic > LIVE_STEPS_PER_PIC else "non-live"
    sim_step = None
    if sim_pic is not None and steps_pic > 0:
        sim_step = sim_pic / steps_pic
    return {"source": "gsplit", "cls": cls, "pics": pics, "steps_pic": steps_pic,
            "metrics": {"sim_step": sim_step, "phys_step": None, "ai_step": None,
                        "render_task": render}}


def parse_present(line):
    m = PRESENT_RE.match(line)
    sm = SMOOTH_RE.search(line)
    if not m or not sm:
        return None
    pics = int(m.group(2))
    if pics < PRESENT_MIN_PICS:
        return None
    return {"source": "present", "cls": "all", "pics": pics, "metrics": {"smooth": float(sm.group(1))}}


def parse_file(path):
    """Return the list of windows in one log file."""
    windows = []
    last_gs = None
    with open(path, "r", errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\r\n")
            if line.startswith("[lane]"):
                w = parse_lane(line)
                last_gs = None
            elif line.startswith("[head]"):
                w = parse_head(line)
                last_gs = None
            elif line.startswith("[gsplit-phys]"):
                m = GS_PHYS_RE.match(line)
                if m and last_gs is not None:
                    last_gs["metrics"]["phys_step"] = num(m.group(1))
                w = None
            elif line.startswith("[gsplit-ai]"):
                m = GS_AI_RE.match(line)
                if m and last_gs is not None:
                    last_gs["metrics"]["ai_step"] = num(m.group(1))
                last_gs = None
                w = None
            elif line.startswith("[gsplit]"):
                w = parse_gsplit(line)
                last_gs = w
            elif line.startswith("[present]"):
                w = parse_present(line)
                last_gs = None
            else:
                continue
            if w is not None:
                windows.append(w)
                if w["source"] != "gsplit":
                    last_gs = None
    # Per-step phase times are only meaningful when the picture had steps.
    for w in windows:
        if w["source"] == "gsplit" and w["steps_pic"] <= 0:
            w["metrics"]["sim_step"] = None
            w["metrics"]["phys_step"] = None
            w["metrics"]["ai_step"] = None
    return windows


def bootstrap_ci(on, off):
    """90 % bootstrap CI of mean(on) - mean(off), fixed seed."""
    rng = random.Random(BOOT_SEED)
    n_on, n_off = len(on), len(off)
    diffs = []
    for _ in range(BOOT_RESAMPLES):
        a = rng.choices(on, k=n_on)
        b = rng.choices(off, k=n_off)
        diffs.append(sum(a) / n_on - sum(b) / n_off)
    diffs.sort()
    return diffs[int(0.05 * BOOT_RESAMPLES)], diffs[int(0.95 * BOOT_RESAMPLES) - 1]


def verdict(on, off, lo, hi, higher_better=False):
    if not on or not off:
        return "no data"
    if len(on) < 2 or len(off) < 2:
        return "too few windows"
    if hi < 0:
        return "OFF better" if higher_better else "ON better"
    if lo > 0:
        return "ON better" if higher_better else "OFF better"
    return "no difference"


def mean(xs):
    return sum(xs) / len(xs) if xs else None


def median(xs):
    if not xs:
        return None
    s = sorted(xs)
    n = len(s)
    mid = n // 2
    return s[mid] if n % 2 else (s[mid - 1] + s[mid]) / 2


def analyse(on_files, off_files, metrics):
    sides = {"on": on_files, "off": off_files}
    values = {}   # (metric, cls) -> {"on": [...], "off": [...]}
    pictures = {"on": {}, "off": {}}
    for side, files in sides.items():
        for path in files:
            for w in parse_file(path):
                src = w["source"]
                pictures[side][src] = pictures[side].get(src, 0) + w["pics"]
                for metric in metrics:
                    if METRIC_SOURCE[metric] != w["source"]:
                        continue
                    value = w["metrics"].get(metric)
                    if value is None:
                        continue
                    slot = values.setdefault((metric, w["cls"]), {"on": [], "off": []})
                    slot[side].append(value)

    rows = []
    for metric in metrics:
        classes = sorted({cls for (m, cls) in values if m == metric},
                         key=lambda c: (c != "play", c))
        for cls in classes:
            on = values[(metric, cls)]["on"]
            off = values[(metric, cls)]["off"]
            row = {"metric": metric, "label": dict((m[0], m[2]) for m in METRICS)[metric],
                   "class": cls, "n_on": len(on), "n_off": len(off),
                   "mean_on": mean(on), "median_on": median(on),
                   "mean_off": mean(off), "median_off": median(off)}
            if on and off:
                row["diff"] = row["mean_on"] - row["mean_off"]
                lo, hi = bootstrap_ci(on, off)
                row["ci90"] = [lo, hi]
            else:
                row["diff"] = None
                row["ci90"] = None
            lo_hi = row["ci90"] or (0.0, 0.0)
            row["verdict"] = verdict(on, off, lo_hi[0], lo_hi[1], metric in HIGHER_BETTER)
            rows.append(row)
    return rows, pictures


def paired_ci(diffs):
    """90 % bootstrap CI of the mean of matched-window differences (on - off), fixed seed."""
    rng = random.Random(BOOT_SEED)
    n = len(diffs)
    means = sorted(sum(rng.choices(diffs, k=n)) / n for _ in range(BOOT_RESAMPLES))
    return means[int(0.05 * BOOT_RESAMPLES)], means[int(0.95 * BOOT_RESAMPLES) - 1]


def analyse_paired(on_files, off_files, metrics):
    """Matched-pair analysis (from Galaxy's tools/console31/abpair.py, 22 Nov 2026): the Nth ON log is paired with the Nth OFF log (run order on, off, on, off: neighbours in
    time, so heat, battery and card drift cancel inside a pair), and inside a pair the Kth window of a class in the ON run with the Kth window of the same class in the OFF run
    (the same scene at the same place in the same route). The unit of the bootstrap is a matched window, its value on - off. Fewer than two matched windows: "too few windows"."""
    n = min(len(on_files), len(off_files))
    diffs = {}    # (metric, cls) -> [on - off], and the matched members
    mem = {}
    pictures = {"on": {}, "off": {}}
    for i in range(n):
        per = {}
        for side, path in (("on", on_files[i]), ("off", off_files[i])):
            for w in parse_file(path):
                src = w["source"]
                pictures[side][src] = pictures[side].get(src, 0) + w["pics"]
                for metric in metrics:
                    if METRIC_SOURCE[metric] != src:
                        continue
                    value = w["metrics"].get(metric)
                    if value is None:
                        continue
                    per.setdefault((metric, w["cls"], side), []).append(value)
        for (metric, cls, side), vals in list(per.items()):
            if side != "on":
                continue
            offv = per.get((metric, cls, "off"), [])
            for k in range(min(len(vals), len(offv))):
                diffs.setdefault((metric, cls), []).append(vals[k] - offv[k])
                m = mem.setdefault((metric, cls), ([], []))
                m[0].append(vals[k]); m[1].append(offv[k])
    rows = []
    label = dict((m[0], m[2]) for m in METRICS)
    for metric in metrics:
        for cls in sorted({c for (m, c) in diffs if m == metric}, key=lambda c: (c != "play", c)):
            d = diffs[(metric, cls)]; on, off = mem[(metric, cls)]
            row = {"metric": metric, "label": label[metric], "class": cls, "n_on": len(d), "n_off": len(d),
                   "mean_on": mean(on), "median_on": median(on), "mean_off": mean(off), "median_off": median(off),
                   "diff": mean(d), "ci90": None}
            if len(d) >= 2:
                lo, hi = paired_ci(d)
                row["ci90"] = [lo, hi]
                row["verdict"] = verdict(on, off, lo, hi)
            else:
                row["verdict"] = "too few windows"
            rows.append(row)
    return rows, pictures


def parse_inrun(path):
    """The measured windows of one MSC_AB run: {(key, window number): {"arm": 0|1, "pics": n, "secs": s, "m": {metric: pics-weighted mean}}}. A window cut in two by the
    300-picture line (or by a log line of its own) is put together again, weighted by pictures. Settle and pre lines are not matched (their tag is ab-settle / ab-pre)."""
    out = {}
    with open(path, "r", errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\r\n")
            if not line.startswith("[lane]"):
                continue
            t = LANE_AB_RE.search(line)
            h = LANE_RATE_RE.match(line)
            w = parse_lane(line.rsplit(" ab ", 1)[0]) if t else None
            if not t or not h or w is None:
                continue
            key, wn, arm = t.group(1), int(t.group(4)), 0 if t.group(3) == "a" else 1
            pics, secs = int(h.group(1)), float(h.group(2))
            slot = out.setdefault((key, wn), {"arm": arm, "pics": 0, "secs": 0.0, "m": {}, "mw": {}, "dc": 0.0})
            slot["pics"] += pics
            slot["secs"] += secs
            slot["dc"] += w["dc"] * pics
            for metric in INRUN_METRICS[1:]:
                v = w["metrics"].get(metric)
                if v is None:
                    continue
                slot["m"][metric] = slot["m"].get(metric, 0.0) + v * pics
                slot["mw"][metric] = slot["mw"].get(metric, 0) + pics
    for slot in out.values():
        slot["dc"] = slot["dc"] / slot["pics"] if slot["pics"] else 0.0
        vals = {"fps": slot["pics"] / slot["secs"] if slot["secs"] > 0 else None}
        for metric, tot in slot["m"].items():
            vals[metric] = tot / slot["mw"][metric]
        slot["v"] = vals
    return out


INRUN_DC_TOL = 0.15


def analyse_inrun(files, metrics, dc_tol=INRUN_DC_TOL):
    """ABA differences: every b window against the mean of the a windows next to it (N-1 and N+1, whichever exist), per key; the bootstrap is over the differences.
    A b window whose draw count a picture (dc) is more than dc_tol (a fraction; 0: off) away from its a neighbours' is a different scene (a goal, a replay, a cut) and is
    dropped, as are neighbours that differ that much: the windows of one scene are the only ones worth comparing. The number dropped is in the result."""
    keys = {}
    dropped = {"scene": 0, "no_neighbour": 0, "used": 0}
    pictures = {"a": 0, "b": 0}
    for path in files:
        wins = parse_inrun(path)
        for (key, wn), slot in wins.items():
            pictures["a" if slot["arm"] == 0 else "b"] += slot["pics"]
        for (key, wn), slot in wins.items():
            if slot["arm"] != 1:
                continue
            nb = [wins[(key, k)] for k in (wn - 1, wn + 1) if (key, k) in wins and wins[(key, k)]["arm"] == 0]
            if not nb:
                dropped["no_neighbour"] += 1
                continue
            if dc_tol > 0:
                ok = [n for n in nb if slot["dc"] > 0 and abs(n["dc"] - slot["dc"]) <= dc_tol * slot["dc"]]
                if len(ok) < len(nb):
                    dropped["scene"] += 1
                    if not ok:
                        continue
                nb = ok
            dropped["used"] += 1
            for metric in metrics:
                b = slot["v"].get(metric)
                a = [n["v"][metric] for n in nb if n["v"].get(metric) is not None]
                if b is None or not a:
                    continue
                rec = keys.setdefault((key, metric), {"d": [], "a": [], "b": []})
                rec["d"].append(b - sum(a) / len(a))
                rec["a"].append(sum(a) / len(a))
                rec["b"].append(b)
    rows = []
    for (key, metric) in sorted(keys, key=lambda km: (km[0], INRUN_METRICS.index(km[1]))):
        rec = keys[(key, metric)]
        d = rec["d"]
        row = {"key": key, "metric": metric, "label": INRUN_LABEL[metric], "n": len(d), "mean_a": mean(rec["a"]), "mean_b": mean(rec["b"]), "diff": mean(d), "ci90": None}
        if len(d) >= 2:
            lo, hi = paired_ci(d)
            row["ci90"] = [lo, hi]
            row["verdict"] = verdict(rec["b"], rec["a"], lo, hi, metric in HIGHER_BETTER)
        else:
            row["verdict"] = "too few windows"
        rows.append(row)
    pictures["dropped"] = dropped
    return rows, pictures


def render_inrun(rows, pictures, files):
    out = [f"abpair --inrun: {len(files)} run(s); b = the candidate (ON), a = the baseline (OFF); diff = b minus the a windows beside it (ms; fps: pictures/s, higher is better)",
           f"pictures in measured windows: a {pictures['a']}, b {pictures['b']}; b windows used {pictures['dropped']['used']}, with a neighbour left out for a scene change (dc differs more than "
           f"{int(INRUN_DC_TOL * 100)} %) {pictures['dropped']['scene']}, without an a neighbour {pictures['dropped']['no_neighbour']}", ""]
    head = f"{'key':<26}{'metric':<24}{'n':>3}{'mean_a':>9}{'mean_b':>9}{'diff':>9}  {'90% CI':<21}verdict"
    out.append(head)
    out.append("-" * len(head))
    for r in rows:
        ci = "-" if r["ci90"] is None else f"[{r['ci90'][0]:+.2f}, {r['ci90'][1]:+.2f}]"
        out.append(f"{r['key']:<26}{r['label']:<24}{r['n']:>3}{fmt_val(r['mean_a']):>9}{fmt_val(r['mean_b']):>9}{r['diff']:>+9.2f}  {ci:<21}{r['verdict']}")
    if not rows:
        out.append("(no matched windows: was the run made with MSC_AB, the [lane] probe on, and at least one b window with an a window beside it?)")
    return "\n".join(out)


def render_table(rows, pictures, on_files, off_files):
    out = []
    out.append(f"abpair: ON {len(on_files)} file(s), OFF {len(off_files)} file(s); "
               "lower is better (smoothness score: higher); diff = ON - OFF in ms (score points)")
    for side in ("on", "off"):
        parts = ", ".join(f"{k} {v}" for k, v in sorted(pictures[side].items()))
        out.append(f"pictures {side.upper()}: {sum(pictures[side].values())} ({parts or 'none'})")
    out.append("")
    head = (f"{'metric':<22}{'class':<10}{'n_on':>5}{'n_off':>6}"
            f"{'mean_on':>9}{'med_on':>8}{'mean_off':>9}{'med_off':>8}"
            f"{'diff':>9}  {'90% CI':<21}verdict")
    out.append(head)
    out.append("-" * len(head))
    for r in rows:
        ci = "-" if r["ci90"] is None else f"[{r['ci90'][0]:+.2f}, {r['ci90'][1]:+.2f}]"
        diff = "-" if r["diff"] is None else f"{r['diff']:+.2f}"
        out.append(f"{r['label']:<22}{r['class']:<10}{r['n_on']:>5}{r['n_off']:>6}"
                   f"{fmt_val(r['mean_on']):>9}{fmt_val(r['median_on']):>8}"
                   f"{fmt_val(r['mean_off']):>9}{fmt_val(r['median_off']):>8}"
                   f"{diff:>9}  {ci:<21}{r['verdict']}")
    if not rows:
        out.append("(no matching windows)")
    return "\n".join(out)


def fmt_val(x):
    return "-" if x is None else f"{x:.2f}"


def main(argv=None):
    ap = argparse.ArgumentParser(description="Compare ON and OFF console log windows.")
    ap.add_argument("--on", nargs="+", metavar="LOG", help="switch ON log files")
    ap.add_argument("--off", nargs="+", metavar="LOG", help="switch OFF log files")
    ap.add_argument("--inrun", nargs="+", metavar="LOG", help="logs of runs made with MSC_AB: the b windows against the a windows beside them, inside each run")
    ap.add_argument("--dc-tol", type=float, default=INRUN_DC_TOL, help="--inrun: leave out an a neighbour whose draw count a picture differs from the b window's by more than this fraction (a different scene); 0: off")
    ap.add_argument("--metric", action="append", default=None,
                    help="metric(s) to report, comma separated or repeated "
                         f"(default all: {', '.join(METRIC_NAMES)})")
    ap.add_argument("--json", action="store_true", help="print JSON instead of the table")
    ap.add_argument("--paired", action="store_true",
                    help="matched pairs: the Nth ON log against the Nth OFF log (run order on, off, on, off), the Kth window of a class "
                         "against the Kth of the same class; the bootstrap is over the differences, so drift between neighbouring runs cancels "
                         "(needs the same number of ON and OFF logs for every run to count)")
    args = ap.parse_args(argv)

    if args.inrun:
        if args.on or args.off or args.paired:
            ap.error("--inrun stands alone (no --on, --off or --paired)")
        im = INRUN_METRICS
        if args.metric:
            requested = [m.strip() for group in args.metric for m in group.split(",") if m.strip()]
            unknown = [m for m in requested if m not in INRUN_METRICS]
            if unknown:
                ap.error(f"unknown --inrun metric(s): {', '.join(unknown)}; choose from {', '.join(INRUN_METRICS)}")
            im = [m for m in INRUN_METRICS if m in requested]
        for path in args.inrun:
            try:
                with open(path, "rb"):
                    pass
            except OSError as exc:
                print(f"abpair: cannot read {path}: {exc}", file=sys.stderr)
                return 2
        rows, pictures = analyse_inrun(args.inrun, im, args.dc_tol)
        if args.json:
            print(json.dumps({"inrun": args.inrun, "metrics": im, "pictures": pictures, "results": rows}, indent=2))
        else:
            print(render_inrun(rows, pictures, args.inrun))
        return 0
    if not args.on or not args.off:
        ap.error("give --on and --off logs, or --inrun logs")

    metrics = METRIC_NAMES
    if args.metric:
        requested = [m.strip() for group in args.metric for m in group.split(",") if m.strip()]
        unknown = [m for m in requested if m not in METRIC_SOURCE]
        if unknown:
            ap.error(f"unknown metric(s): {', '.join(unknown)}; "
                     f"choose from {', '.join(METRIC_NAMES)}")
        metrics = [m for m in METRIC_NAMES if m in requested]

    for path in args.on + args.off:
        try:
            with open(path, "rb"):
                pass
        except OSError as exc:
            print(f"abpair: cannot read {path}: {exc}", file=sys.stderr)
            return 2

    if args.paired and len(args.on) != len(args.off):
        print(f"abpair: --paired needs as many ON as OFF logs ({len(args.on)} against {len(args.off)}): only the first {min(len(args.on), len(args.off))} pairs are used", file=sys.stderr)
    rows, pictures = (analyse_paired if args.paired else analyse)(args.on, args.off, metrics)
    if args.json:
        doc = {"on_files": args.on, "off_files": args.off, "metrics": metrics,
               "pictures": pictures, "results": rows}
        print(json.dumps(doc, indent=2))
    else:
        print(render_table(rows, pictures, args.on, args.off))
    return 0


if __name__ == "__main__":
    sys.exit(main())
