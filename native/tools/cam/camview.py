#!/usr/bin/env python3
"""W5-CAM: what the match camera shows, from the MSC_CAMERA_LOG lines of a run (np/camzoom.h).

    python3 -I tools/cam/camview.py frames <run.log> [frame ...]        one line per frame: zoom, FOV, visible pitch, on-screen checks
    python3 -I tools/cam/camview.py moments <run.log> [--json out.json]  pick the frames of the brief's moments from a run (see pick_moments)
    python3 -I tools/cam/camview.py stats <run.log> [<run.log> ...]      open-play averages per run (visible area, length, width, misses)
    python3 -I tools/cam/camview.py same <a.log> <b.log>                 do two runs show the same match? (ball, players, remap angle per frame)

The projection is the game's own: the [cam] line carries the view matrix the frame used (cCameraManager::m_matView, row-vector
convention as glplatMatrixLookAt writes it), the vertical FOV in degrees and the aspect ratio (1.666 wide); C_MTXPerspective.
The camera looks down -z of camera space. A point is on screen when |x_ndc| <= 1 and |y_ndc| <= 1 and it is in front of the camera.
The pitch is the rectangle |x| <= fd.x, |y| <= fd.y on z = 0 (cField::mv3FieldPosition: 20.6 x 12.5, so 41.2 m long, 25 m wide).
"""
import json
import math
import re
import sys

SCREEN_W, SCREEN_H = 960, 544
LINE = re.compile(r"^\[cam\] (.*)$")


def parse(path):
    frames = {}
    with open(path, "rb") as f:
        for raw in f:
            if not raw.startswith(b"[cam] "):
                continue
            line = raw.decode("latin-1").rstrip("\n")
            d = {}
            for tok in line[6:].split(" "):
                if "=" not in tok:
                    continue
                k, v = tok.split("=", 1)
                d[k] = v
            try:
                fr = int(d["f"])
            except (KeyError, ValueError):
                continue
            rec = {"f": fr, "top": int(d.get("top", -1)), "tr": int(d.get("tr", 0)), "fov": float(d["fov"]), "asp": float(d["asp"]),
                   "rm": int(d.get("rm", 0)), "auto": int(d.get("auto", -1)), "lvl": float(d.get("lvl", -1)), "ds": d.get("ds")}
            for k in ("z", "dz", "gfov", "zs"):
                if k in d:
                    rec[k] = float(d[k])
            for k in ("force", "trn"):
                if k in d:
                    rec[k] = int(d[k])
            if "fl" in d:
                rec["fl"] = int(d["fl"], 16)
            fx, fy = (float(x) for x in d.get("fd", "20.6,12.5").split(","))
            rec["fd"] = (fx, fy)
            V = [float(x) for x in d["V"].split(",")]
            rec["V"] = [V[0:4], V[4:8], V[8:12], V[12:16]]
            if "b" in d:
                rec["b"] = tuple(float(x) for x in d["b"].split(","))
                players = []
                for p in d["P"].split(";"):
                    players.append(None if p == "-" else tuple(float(x) for x in p.split(",")))
                rec["P"] = players
                rec["own"] = int(d.get("own", -1))
                rec["ctl"] = [int(c) for c in d.get("ctl", "-") if c.isdigit()]
                rec["vis"] = int(d.get("vis", "0"), 16)
                rec["dvis"] = int(d.get("dvis", "0"), 16)
            frames[fr] = rec
    return frames


def to_camera(rec, p):
    V = rec["V"]
    x, y, z = p
    return [x * V[0][j] + y * V[1][j] + z * V[2][j] + V[3][j] for j in range(3)]


def project(rec, p):
    """(u, v) in screen pixels of a 960x544 picture (u right, v down), or None behind the camera."""
    c = to_camera(rec, p)
    depth = -c[2]
    if depth <= 0.25:
        return None
    cot = 1.0 / math.tan(math.radians(rec["fov"]) * 0.5)
    xn = cot / rec["asp"] * c[0] / depth
    yn = cot * c[1] / depth
    return ((xn + 1.0) * 0.5 * SCREEN_W, (1.0 - yn) * 0.5 * SCREEN_H)


def on_screen(rec, p, margin=0.0):
    s = project(rec, p)
    if s is None:
        return False
    u, v = s
    return margin <= u <= SCREEN_W - margin and margin <= v <= SCREEN_H - margin


def edge_distance(rec, p):
    """pixels from the nearest screen edge (negative = off screen by that much)"""
    s = project(rec, p)
    if s is None:
        return -9999.0
    u, v = s
    return min(u, SCREEN_W - u, v, SCREEN_H - v)


def visible_pitch(rec, step=0.25):
    """fraction of the pitch area on screen; visible share of the pitch length on the ball's line (y = ball y) and of its width on the
    ball's line (x = ball x), both clipped to the pitch."""
    fx, fy = rec["fd"]
    nx, ny = int(2 * fx / step), int(2 * fy / step)
    seen = 0
    for i in range(nx):
        x = -fx + (i + 0.5) * step
        for j in range(ny):
            y = -fy + (j + 0.5) * step
            if on_screen(rec, (x, y, 0.0)):
                seen += 1
    area = seen / float(nx * ny)
    b = rec.get("b", (0.0, 0.0, 0.0))
    by = max(-fy, min(fy, b[1]))
    bx = max(-fx, min(fx, b[0]))
    n = 400
    length = sum(1 for i in range(n) if on_screen(rec, (-fx + (i + 0.5) * 2 * fx / n, by, 0.0))) / float(n)
    width = sum(1 for i in range(n) if on_screen(rec, (bx, -fy + (i + 0.5) * 2 * fy / n, 0.0))) / float(n)
    return area, length, width


def people(rec):
    """the action around the ball: the ball; the controlled player when he is within 12 m of the ball (a scripted run often leaves him
    behind after a pass); the two players of his team nearest to the ball (or of the ball carrier's team) and the two opponents nearest to it"""
    out = []
    if "b" not in rec:
        return out
    P = rec["P"]
    b = rec["b"]
    out.append(("ball", b))

    def dist(p):
        return math.hypot(p[0] - b[0], p[1] - b[1])

    ctl = [c for c in rec["ctl"] if c < 10 and P[c] is not None]
    team = (ctl[0] // 5) if ctl else (rec["own"] // 5 if rec.get("own", -1) >= 0 else 0)
    skip = -1
    if ctl and dist(P[ctl[0]]) < 12.0:
        out.append(("ctl", P[ctl[0]]))
        skip = ctl[0]
    mates = sorted([P[i] for i in range(team * 5, team * 5 + 5) if P[i] is not None and i != skip], key=dist)[:2]
    opps = sorted([P[i] for i in range((1 - team) * 5, (1 - team) * 5 + 5) if P[i] is not None], key=dist)[:2]
    out += [("mate", p) for p in mates] + [("opp", p) for p in opps]
    return out


def checks(rec):
    """per person: on screen (feet and head, 1.6 m), and the smallest edge distance in pixels"""
    res = []
    for name, p in people(rec):
        pts = [p] if name == "ball" else [p, (p[0], p[1], p[2] + 1.6)]
        d = min(edge_distance(rec, q) for q in pts)
        res.append((name, d >= 0, d))
    return res


def describe(rec):
    area, length, width = visible_pitch(rec)
    ch = checks(rec)
    miss = [n for n, ok, _ in ch if not ok]
    worst = min((d for _, _, d in ch), default=0)
    return {"f": rec["f"], "z": rec.get("z"), "fov": rec["fov"], "area": area, "length": length, "width": width, "miss": miss,
            "edge": worst, "fl": rec.get("fl", 0), "b": rec.get("b"), "own": rec.get("own"), "lvl": rec["lvl"], "auto": rec["auto"]}


def fmt(d):
    b = d["b"] or (0, 0, 0)
    return ("f=%d lvl=%d auto=%d z=%.3f fov=%.2f area=%.0f%% length=%.0f%% width=%.0f%% fl=%02x ball=%.1f,%.1f own=%s off=%s edge=%.0f"
            % (d["f"], int(4 * d["lvl"]) + 1, d["auto"], d["z"] if d["z"] is not None else -1, d["fov"], 100 * d["area"], 100 * d["length"],
               100 * d["width"], d["fl"], b[0], b[1], d["own"], ",".join(d["miss"]) or "-", d["edge"]))


def is_play(rec):
    return rec["top"] == 1 and rec["tr"] == 0 and rec.get("trn", 0) == 0 and rec.get("force", 0) == 0 and "b" in rec


def runs_of(frames, pred, min_len):
    """contiguous runs (by frame number) of frames satisfying pred, at least min_len long: list of (first, last)"""
    out = []
    start = prev = None
    for f in sorted(frames):
        if pred(frames[f]):
            if start is not None and f == prev + 1:
                prev = f
                continue
            if start is not None and prev - start + 1 >= min_len:
                out.append((start, prev))
            start = prev = f
        else:
            if start is not None and prev - start + 1 >= min_len:
                out.append((start, prev))
            start = prev = None
    if start is not None and prev - start + 1 >= min_len:
        out.append((start, prev))
    return out


def pick_moments(frames, attack_sign, open_play):
    """attack_sign: +1 when the home team (pad 0) attacks +x. open_play: the frame of 'STATE open play'.
    Returns {name: frame}: the middle of the first long enough run of frames that fits each moment (or of the given window)."""
    fx = 20.6
    picks = {}

    def home_owns(r):
        return 0 <= r["own"] < 5

    def away_owns(r):
        return 5 <= r["own"] < 10

    def calm(r):   # no shot wind-up and no Mega Strike meter (they have moments of their own)
        return is_play(r) and (r.get("fl", 0) & 0x0e) == 0

    cands = {
        "kickoff": None,
        "midfield": lambda r: calm(r) and home_owns(r) and abs(r["b"][0]) < 6 and abs(r["b"][1]) < 7,
        "attack_far_goal": lambda r: calm(r) and home_owns(r) and attack_sign * r["b"][0] > fx - 9 and abs(r["b"][1]) < 9,
        "attack_own_goal": lambda r: calm(r) and away_owns(r) and -attack_sign * r["b"][0] > fx - 9 and abs(r["b"][1]) < 9,
        "touchline_far": lambda r: calm(r) and r["own"] >= 0 and r["b"][1] > 9.0 and abs(r["b"][0]) < fx - 4,
        "touchline_near": lambda r: calm(r) and r["own"] >= 0 and r["b"][1] < -9.0 and abs(r["b"][0]) < fx - 4,
        "megastrike": None,
    }
    # the Mega Strike set-up: the captain's charge (wind-up flag 2) just before the meter starts (flag 8), else the meter's first frames
    meter = runs_of(frames, lambda r: r["top"] == 1 and (r.get("fl", 0) & 8) != 0, 10)
    best = None
    for a0, _ in meter:
        windup = [f for f in range(a0 - 40, a0) if f in frames and (frames[f].get("fl", 0) & 2)]
        f = windup[len(windup) // 2] if windup else a0 + 5
        ours = 0 <= frames[f].get("own", -1) < 5
        if best is None or (ours and not best[1]):
            best = (f, ours)
    if best:
        picks["megastrike"] = best[0]
    for name, pred in cands.items():
        if name == "megastrike":
            continue
        if name == "kickoff":
            later = [f for f in sorted(frames) if f >= open_play + 30 and frames[f]["top"] == 1]
            if later:
                picks[name] = later[0]
            continue
        rs = runs_of(frames, pred, 20)
        if rs:
            # the longest run; within it a frame 2/3 of the way in (the zoom has eased by then)
            a, b = max(rs, key=lambda r: r[1] - r[0])
            picks[name] = a + (b - a) * 2 // 3
    return picks


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "frames":
        frames = parse(argv[2])
        sel = [int(x) for x in argv[3:]] or sorted(frames)
        for f in sel:
            if f in frames:
                print(fmt(describe(frames[f])))
            else:
                print("f=%d: no [cam] line" % f)
        return 0
    if cmd == "stats":
        for path in argv[2:]:
            frames = parse(path)
            play = [frames[f] for f in sorted(frames) if is_play(frames[f])]
            sample = play[::10]
            if not sample:
                print("%s: no open-play frames" % path)
                continue
            ds = [describe(r) for r in sample]
            n = float(len(ds))
            miss = {}
            for d in ds:
                for m in d["miss"]:
                    miss[m] = miss.get(m, 0) + 1
            zs = sorted(d["z"] for d in ds)
            onscr = sum(bin(r.get("vis", 0)).count("1") for r in sample) / n
            print("%s: %d open-play frames sampled (every 10th): level %d auto %d; zoom median %.3f (p10 %.3f, p90 %.3f); FOV mean %.2f; pitch area "
                  "%.1f%%, length %.1f%%, width %.1f%% (means); players on screen (the game's own test) %.2f of 10; ball/ctl/2 mates/2 opps off screen: %s; frames with one of them off: %.1f%%"
                  % (path, len(ds), int(4 * ds[0]["lvl"]) + 1, ds[0]["auto"], zs[len(zs) // 2], zs[len(zs) // 10], zs[9 * len(zs) // 10],
                     sum(d["fov"] for d in ds) / n, 100 * sum(d["area"] for d in ds) / n, 100 * sum(d["length"] for d in ds) / n,
                     100 * sum(d["width"] for d in ds) / n, onscr, ", ".join("%s %.1f%%" % (k, 100 * v / n) for k, v in sorted(miss.items())) or "nobody",
                     100 * sum(1 for d in ds if d["miss"]) / n))
        return 0
    if cmd == "same":
        a, b = parse(argv[2]), parse(argv[3])
        common = sorted(set(a) & set(b))
        diff = [f for f in common if a[f].get("b") != b[f].get("b") or a[f].get("P") != b[f].get("P")]
        rdiff = [f for f in common if a[f]["rm"] != b[f]["rm"]]
        sdiff = [f for f in common if a[f]["ds"] != b[f]["ds"]]
        vdiff = [f for f in common if a[f].get("vis") != b[f].get("vis")]
        ddiff = [f for f in common if a[f].get("dvis") != b[f].get("dvis")]
        print("%d common frames; ball/players differ in %d (first %s); remap angle differs in %d (first %s); presentation seed nlDefaultSeed "
              "differs in %d (first %s); rendered on-screen flags differ in %d (first %s); simulation's on-screen flags differ in %d (first %s)"
              % (len(common), len(diff), diff[0] if diff else "-", len(rdiff), rdiff[0] if rdiff else "-", len(sdiff), sdiff[0] if sdiff else "-",
                 len(vdiff), vdiff[0] if vdiff else "-", len(ddiff), ddiff[0] if ddiff else "-"))
        return 1 if diff else 0
    if cmd == "moments":
        frames = parse(argv[2])
        open_play = 0
        with open(argv[2], "rb") as f:
            for raw in f:
                if b"STATE open play" in raw and raw.startswith(b"[script]"):
                    m = re.search(rb"f=(\d+)", raw)
                    open_play = int(m.group(1))
                    break
        # which way does the home team attack? the side its players stand on at the first logged kick-off frame
        sign = 1
        for f in sorted(frames):
            r = frames[f]
            if "P" in r and r["P"][0] is not None and f >= open_play:
                home_x = sum(p[0] for p in r["P"][:5] if p) / 5.0
                sign = 1 if home_x < 0 else -1
                break
        picks = pick_moments(frames, sign, open_play)
        print("open play at f=%d; the home team attacks %sx" % (open_play, "+" if sign > 0 else "-"))
        for k, f in picks.items():
            print("%-16s %s" % (k, fmt(describe(frames[f]))))
        if "--json" in argv:
            with open(argv[argv.index("--json") + 1], "w") as out:
                json.dump({"open_play": open_play, "attack_sign": sign, "picks": picks}, out, indent=1)
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
