#!/usr/bin/env python3
"""Read the logs of a console session and print one table: frame time and fps per scene per run, against the defaults and the noise.

    python3 analyse.py <session dir> [--refs d1,d2] [--window 10] [--csv table.csv] [--json table.json] [--runs id,id,...]
    python3 analyse.py --lan <host log> <join log>

<session dir> holds one folder per run, <run id>/log.txt (what session.sh pulls); a flat <run id>.log works too. plan.json (next to this
script) tells the roles (reference, version, look) and the order; --refs overrides the two reference runs.

What it reads, and the code that prints it
  [frame-timing] reason=.. first_frame=.. n=.. total_us=.. p50_us=.. p95_us=.. p99_us=.. scene=.. gs=.. task=.. lat_*   native/platform/harness/src/np_harness.cpp (EmitTiming)
        one line per window of 300 frames (MSC_FRAME_TIMING=300) or at a scene change: the percentiles of the intervals between frames
  [script] f=N @begin <scene> / @end <scene>                                                                           the kit's routes (gen_routes.py)
  [vgx] frame F: N shader compiles / [vgx-vita] present P came X ms after the previous one: +V vertex shaders ..       renderer/src/vgx_backend_gl.cpp, renderer/vita/vgx_host_vita.cpp
  [vgx-vita] present P: / shader work so far: N compiles ...  (every 600 presents)                                    the same files: the compile counter between two lines
  [run] ... clocks ARM .. MHz, fps=, dynres=, gfx:, settings:, controls:                                              native/platform/vita/src/np_vita_platform.cpp (LogRunHeader)
  [fps] auto: 30 -> 60 ..., [fps] summary:, [vita] frame N: .. audio underruns, [vgx-vita] display: ..                  frame-rate controller, platform, renderer

Pass rules (README.md)
  * a steady window is 10 s of one scene class in a row: one 300-frame window at 30 fps, two at 60 fps (their percentiles are averaged,
    weighted by frames; the fps is exact). Windows cut short by a scene change are not steady windows.
  * a window that contains a shader compile is left out: a compile line inside it (+-6 frames) or a rise of the compile counter between
    the two 600-present lines around it (--keep-suspect keeps the second kind).
  * scenes that never last 10 s (goal replay, celebration) are pooled over every record of the class, marked "~" in the window column.
  * noise = 2 x the spread of the two reference runs (their difference), at least --floor-ms / --floor-fps. A delta bigger than the noise
    is marked: "*" better, "!" worse. Unmarked deltas are within the noise.
"""
import argparse
import csv
import json
import re
import statistics
import sys
from collections import OrderedDict, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOL_FRAMES = 6            # a compile line this close to a window's frames counts as inside it
MARK_CLASSES = {"menu-main", "stadium-select", "walkout", "play", "mega-save", "pipe-walkout", "pipe-play", "lava-walkout", "lava-play"}   # marks that name the scene class
STADIUM_PLAY = {"pipe-play": "pipe", "lava-play": "lava"}   # a stadium route's open-play mark: its cut-scenes and replays get rows of their own (pipe:nis, lava:replay)
SHORT_MIN_SECONDS = 2.0   # a pooled short scene needs at least this much video
METRICS = (("p50", "p50 ms"), ("p95", "p95 ms"), ("p99", "p99 ms"), ("fps", "fps"))
# The scenes each route reports (a run's other records, the title, the menu walk and the loading screens, are real but not a measurement).
ROUTE_CLASSES = {"tour": ("menu-main", "stadium-select", "walkout", "play", "goal:nis", "goal:replay"),
                 "mega": ("mega-save",),
                 "look": ("walkout", "play"),
                 "pipe": ("pipe-walkout", "pipe-play", "pipe:nis", "pipe:replay"),   # (a route's scenes have their own rows: another stadium is not the tour's play)
                 "ctrl": ("pipe-walkout", "pipe-play"),
                 "replay": ("rep:nis", "rep:replay"),
                 # the core session's routes (gen_routes.py ctour, cpipe, clava, crep): the same scenes, shorter
                 "ctour": ("menu-main", "stadium-select", "walkout", "play", "goal:nis", "goal:replay"),
                 "cpipe": ("pipe-walkout", "pipe-play", "pipe:nis", "pipe:replay"),
                 "clava": ("lava-walkout", "lava-play", "lava:nis", "lava:replay"),
                 "crep": ("rep:nis", "rep:replay"),
                 # the overnight session's routes (night.py): the same scenes with a warm-up in front of the live windows
                 "ntour": ("menu-main", "stadium-select", "walkout", "play", "goal:nis", "goal:replay"),
                 "npipe": ("pipe-walkout", "pipe-play", "pipe:nis", "pipe:replay"),
                 "nlava": ("lava-walkout", "lava-play", "lava:nis", "lava:replay")}
DEFAULT_FLOORS = {"p50": 0.25, "p95": 0.5, "p99": 1.0, "fps": 0.3}   # the least noise ever assumed (ms, ms, ms, fps)

KV = re.compile(r"(\w+)=(\S+)")
RE_BUILD = re.compile(r"^\[vita\] =+ .*build (.*?) =+")
RE_SCRIPT = re.compile(r"^\[script\] f=(\d+) (.*)$")
RE_CAME = re.compile(r"^\[vgx-vita\] present (\d+) came (\d+) ms after the previous one: \+(\d+) vertex shaders, \+(\d+) fragment shaders, \+(\d+) programs")
RE_PRESENT = re.compile(r"^\[vgx-vita\] present (\d+): ")
RE_WORK = re.compile(r"^\[vgx-vita\] shader work so far: (\d+) compiles, (\d+) links, (\d+) ms")
RE_VGXFRAME = re.compile(r"^\[vgx\] frame (\d+): (\d+) shader compiles")
RE_AUTO = re.compile(r"^\[fps\] auto: (\w+) -> (\w+) at t=([\d.]+) s, frame (\d+)")
RE_DYN = re.compile(r"^\[vgx\] dynamic resolution: frame (\d+) level (\d+) -> (\d+)")
RE_VITAFRAME = re.compile(r"^\[vita\] frame (\d+): ([\d.]+) fps, game thread ([\d.]+) ms CPU/frame, audio underruns (\d+), dropped (\d+)")
RE_DISPLAY = re.compile(r"^\[vgx-vita\] display: interval (\d+) vblank\(s\), the last (\d+) pictures were shown for 0/1/2/3/4\+ vblanks: (\d+)/(\d+)/(\d+)/(\d+)/(\d+) \(all so far (\d+): (\d+)/(\d+)/(\d+)/(\d+)/(\d+)")
RE_SUMMARY = re.compile(r"^\[fps\] summary: mode=(\d+) frames=(\d+) \(game (\d+), 30 (\d+), 60 (\d+), unlocked (\d+), cut-scene lock (\d+)\)")
RE_CLOCKS = re.compile(r"clocks:? (?:ARM|CPU) (\d+) MHz, (?:GPU (\d+) MHz, bus (\d+)|bus (\d+) MHz, GPU (\d+))")
RE_FATAL = re.compile(r"FATAL|CRASH|\[script\] TIMEOUT|stall: no frame|\[note |\[fatal ")
# The core session's extra columns (README.md "Core session"). Each line is attached to the [frame-timing] record printed last before it (log order):
#   [lane] 300 pics 6.01s 49.9/s game 14.2 send 3.1 fe 12.4 be 7.3/9.8 gpu 18.2 cores 71/63/58/4 ...   renderer/src/vgx_workload.h (MSC_VGX_WORKLOAD_PROBE, on in logging builds)
#   [rt] N frames: ... GPU (n of the last frames timed) busy p50 X p95 Y ms, ...                           renderer/vita/vgx_host_vita.cpp (MSC_VITA_RT_TIMERS)
#   [vgx] size change to WxH / [vgx-vita] MSC_VITA_RENDER: internal picture WxH                            renderer/src/vgx_backend_gl.cpp, vgx_host_vita.cpp
#   [hitch] gap=G cause ...                                                                                 renderer/src/vgx_backend_gl.cpp (MSC_HITCH_MS)
#   [vita] frame N: ... C heap K KiB in use (E KiB taken from the system) / [mem] C heap: block B KiB ...   np_vita_platform.cpp, vita_memreport.cpp
RE_LANE = re.compile(r"^\[lane\] (\d+) pics [\d.]+s ([\d.]+)/s game (\S+) send (\S+) fe (\S+) be (\S+) gpu (\S+)(?: cores (\S+))?(?: dc (\d+))?")
RE_LOCK60 = re.compile(r"^\[lock60\] (\S+) ended: (\d+) measured frames, (\d+) over the 60 budget \(([\d.]+)%\), steps down (\d+) up (\d+), rungs (-?\d+)\.\.(-?\d+)")
RE_RT_SCENES = re.compile(r"^\[rt\] \d+ frames: scenes ([\d.]+)/frame")
RE_RT_GPU = re.compile(r"^\[rt\] \d+ frames: .*GPU \(\d+ of the last frames timed\) busy p50 ([\d.]+) p95 ([\d.]+) ms")
RE_SIZE = re.compile(r"^\[vgx\] size change to (\d+)x(\d+)|^\[vgx-vita\] MSC_VITA_RENDER: internal picture (\d+)x(\d+)")
RE_HITCH = re.compile(r"^\[hitch\] gap=([\d.]+) (\w+)")
RE_HEAP_FRAME = re.compile(r"C heap (\d+) KiB in use \((\d+) KiB taken from the system\)")
RE_HEAP_MEM = re.compile(r"^\[mem\] C heap: block (\d+) KiB .*in use now (\d+) KiB, most ever in use (\d+) KiB.*extent reached (\d+) KiB")
RE_HEAP_PEAK = re.compile(r"C heap (\d+) KiB in use, peak (\d+), of (\d+)")
RE_HEAP_LINE = re.compile(r"C heap in use (\d+) KiB \(extent (\d+) KiB(?: of (\d+))?")
RE_DONE = re.compile(r"script exit|settings screen done|\[settings\] test script finished")
EFB_SIZE = (640, 448)     # the game's own picture size: the size a run starts at until a size line says otherwise


class Run:
    def __init__(self, rid):
        self.id = rid
        self.build = ""
        self.header = []          # [run] lines, first block only
        self.clocks = None        # (arm, gpu, bus)
        self.marks = []           # (frame, text)
        self.records = []         # [frame-timing] reason=window/identity/reset/end records
        self.compile_frames = []  # frames with a compile line (+ shader counts) in the log
        self.counters = []        # (present, compiles) from the 600-present lines
        self.auto_flips = []      # frames of [fps] auto switches
        self.dyn_changes = []
        self.vita_frames = []     # (frame, fps, cpu_ms, underruns, dropped)
        self.display = []         # last 'all so far' tuple of the display lines
        self.fps_summary = None
        self.problems = []
        self.lines = 0
        self._unexplained = None
        # core-session extras: lists of (record index the line follows, values)
        self.lanes, self.rt_gpu, self.hitches, self.rt_scenes = [], [], [], []
        self.lock60 = []          # (scene, measured frames, frames over the 60 budget, percent over, steps down, steps up) of the [lock60] ... ended lines
        self.heap_series = []     # every C-heap "in use" reading, in log order (KiB)
        self.display_win = []     # (record index, interval in vblanks, pictures, shown for 0/1/2/3/4+ vblanks) of the [vgx-vita] display lines (every 600 pictures)
        self.record_sizes = []    # the picture size in force when each record was printed
        self.heap = {"in_use": 0, "extent": 0, "peak": 0, "block": 0}
        self.done = False         # the route reached its end (script exit, or the settings screen's end)

    # ---- parsing ---------------------------------------------------------------------------------------------------------------------
    @classmethod
    def parse(cls, rid, text):
        r = cls(rid)
        lines = text.splitlines()
        start = 0
        for i, l in enumerate(lines):          # the last "===== ... build" line begins this run (the log keeps earlier runs)
            if RE_BUILD.match(l):
                start = i
        r.lines = len(lines) - start
        header_done = False
        last_present = None
        size = EFB_SIZE
        for l in lines[start:]:
            if RE_DONE.search(l):
                r.done = True
            if "C heap" in l:
                r._heap_line(l)
            if not l.startswith("["):
                continue
            m = RE_SIZE.match(l)
            if m:
                g = m.groups()
                size = (int(g[0] or g[2]), int(g[1] or g[3]))
                continue
            m = RE_LANE.match(l)
            if m:
                num = lambda s: float(s.split("/")[0]) if s not in ("-", None) else None
                r.lanes.append((len(r.records) - 1, {"fps": float(m.group(2)), "game": num(m.group(3)), "send": num(m.group(4)), "fe": num(m.group(5)),
                                                     "be": num(m.group(6)), "gpu": num(m.group(7)), "cores": m.group(8) or "-", "dc": float(m.group(9)) if m.group(9) else None}))
                continue
            m = RE_LOCK60.match(l)
            if m:
                r.lock60.append((m.group(1), int(m.group(2)), int(m.group(3)), float(m.group(4)), int(m.group(5)), int(m.group(6))))
                continue
            m = RE_RT_SCENES.match(l)
            if m:
                r.rt_scenes.append((len(r.records) - 1, float(m.group(1))))     # (the same line also carries the GPU busy time: RE_RT_GPU below reads it)
            m = RE_RT_GPU.match(l)
            if m:
                r.rt_gpu.append((len(r.records) - 1, float(m.group(1)), float(m.group(2))))
                continue
            m = RE_HITCH.match(l)
            if m:
                r.hitches.append((len(r.records) - 1, float(m.group(1)), m.group(2)))
                continue
            m = RE_BUILD.match(l)
            if m:
                r.build = m.group(1)
                continue
            if l.startswith("[run] "):
                if not header_done:
                    r.header.append(l[6:])
                    if "first frame" in l or r.clocks is None:
                        c = RE_CLOCKS.search(l)
                        if c:
                            g = c.groups()
                            r.clocks = (int(g[0]), int(g[1] or g[4] or 0), int(g[2] or g[3] or 0))
                continue
            if l.startswith("[vita] clocks:") and r.clocks is None:
                c = RE_CLOCKS.search(l)
                if c:
                    g = c.groups()
                    r.clocks = (int(g[0]), int(g[4] or g[1] or 0), int(g[3] or g[2] or 0))
                continue
            if l.startswith("[frame-timing] "):
                kv = dict(KV.findall(l))
                try:
                    rec = {k: int(kv[k], 16 if k in ("top", "game") else 10) for k in
                           ("first_frame", "last_frame", "n", "total_us", "p50_us", "p95_us", "p99_us", "max_us", "min_us", "scene", "gs", "task", "paused")}
                except (KeyError, ValueError):
                    r.problems.append("unreadable [frame-timing] line")
                    continue
                rec["reason"] = kv.get("reason", "?")
                rec["top"], rec["game"] = int(kv.get("top", "0"), 16), int(kv.get("game", "0"), 16)
                rec["lat_n"] = int(kv.get("lat_n", 0))
                rec["lat_p50_us"] = int(kv.get("lat_p50_us", 0))
                rec["over_33333"] = int(kv.get("over_33333", 0))
                r.records.append(rec)
                r.record_sizes.append(size)
                header_done = True
                continue
            m = RE_SCRIPT.match(l)
            if m:
                header_done = True
                if "@begin " in m.group(2) or "@end " in m.group(2):
                    r.marks.append((int(m.group(1)), m.group(2).strip()))
                elif "TIMEOUT" in m.group(2):
                    r.problems.append("script timeout: " + m.group(2)[:80])
                continue
            m = RE_CAME.match(l)
            if m:
                if int(m.group(3)) + int(m.group(4)) + int(m.group(5)) > 0:
                    r.compile_frames.append(int(m.group(1)))
                continue
            m = RE_VGXFRAME.match(l)
            if m:
                r.compile_frames.append(int(m.group(1)))
                continue
            m = RE_PRESENT.match(l)
            if m:
                last_present = int(m.group(1))
                continue
            m = RE_WORK.match(l)
            if m:
                if last_present is not None:
                    r.counters.append((last_present, int(m.group(1))))
                continue
            m = RE_AUTO.match(l)
            if m:
                r.auto_flips.append(int(m.group(4)))
                continue
            m = RE_DYN.match(l)
            if m:
                r.dyn_changes.append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
                continue
            m = RE_VITAFRAME.match(l)
            if m:
                r.vita_frames.append((int(m.group(1)), float(m.group(2)), float(m.group(3)), int(m.group(4)), int(m.group(5))))
                continue
            m = RE_DISPLAY.match(l)
            if m:
                g = [int(x) for x in m.groups()]
                r.display.append(tuple(g[7:13]))
                r.display_win.append((len(r.records) - 1, g[0], g[1], g[2:7]))
                continue
            m = RE_SUMMARY.match(l)
            if m:
                r.fps_summary = tuple(int(x) for x in m.groups())
                continue
            if RE_FATAL.search(l):
                r.problems.append(l[:100])
        r.counters.sort()
        return r

    def _heap_line(self, l):
        h = self.heap
        seen = RE_HEAP_MEM.search(l) or RE_HEAP_PEAK.search(l) or RE_HEAP_LINE.search(l) or RE_HEAP_FRAME.search(l)
        if seen:
            self.heap_series.append(int(seen.group(2) if seen.re is RE_HEAP_MEM else seen.group(1)))
        for rx, fields in ((RE_HEAP_MEM, ("block", "in_use", "peak", "extent")), (RE_HEAP_PEAK, ("in_use", "peak", "block")),
                           (RE_HEAP_LINE, ("in_use", "extent", "block")), (RE_HEAP_FRAME, ("in_use", "extent"))):
            m = rx.search(l)
            if m:
                for name, value in zip(fields, m.groups()):
                    if value is not None:
                        h[name] = int(value) if name == "block" else max(h[name], int(value))
                h["peak"] = max(h["peak"], h["in_use"])
                return

    # ---- scenes ----------------------------------------------------------------------------------------------------------------------
    def mark_intervals(self):
        out, open_ = [], {}
        for f, text in self.marks:
            kind, _, name = text.partition(" ")
            if kind == "@begin":
                open_[name] = f
            elif kind == "@end" and name in open_:
                out.append((open_.pop(name), f, name))
        for name, f in open_.items():           # a scene that never ended (the run was cut): runs to the last frame
            out.append((f, 10 ** 12, name))
        return out

    def classify(self, rec, intervals):
        mid = (rec["first_frame"] + rec["last_frame"]) // 2
        derived = derived_class(rec)
        for a, b, name in intervals:
            if a <= mid <= b:
                if name == "play":
                    # a cut-scene or replay inside open play is a goal sequence (either side), whatever the route was doing at the time
                    return "goal:" + derived if derived in ("nis", "replay") else derived
                if name in STADIUM_PLAY:
                    return name if derived == "play" else STADIUM_PLAY[name] + ":" + derived
                if name == "rep-sequence":
                    return "rep:" + derived
                if name in MARK_CLASSES:
                    return name
                return "goal:" + derived
        return derived

    def unexplained_blocks(self):
        """(from present, to present) of the 600-present spans in which the compile counter rose but no compile line says where."""
        out = []
        for (p0, c0), (p1, c1) in zip(self.counters, self.counters[1:]):
            if c1 > c0 and not any(p0 - TOL_FRAMES <= f <= p1 + TOL_FRAMES for f in self.compile_frames):
                out.append((p0, p1))
        return out

    def tainted(self, rec, keep_suspect):
        a, b = rec["first_frame"], rec["last_frame"]
        for f in self.compile_frames:
            if a - TOL_FRAMES <= f <= b + TOL_FRAMES:
                return "compile"
        if not keep_suspect:
            if self._unexplained is None:
                self._unexplained = self.unexplained_blocks()
            for p0, p1 in self._unexplained:
                if a < p1 + TOL_FRAMES and b > p0 - TOL_FRAMES:
                    return "suspect"
        return ""


def derived_class(rec):
    task, scene, gs = rec["task"], rec["scene"], rec["gs"]
    if task & 0xFFFF0000:
        return "load"
    if task & 0x10:
        return "nis"
    if task & 0x8:
        return "replay"
    if task == 4:
        return {1: "menu-main", 5: "stadium-select"}.get(scene, "menu-other")
    if rec["paused"]:
        return "pause"
    if task & 0x3:
        return "play" if gs == 5 else "play-gs%d" % gs
    return "other"


# ---- windows -------------------------------------------------------------------------------------------------------------------------

def identity(rec):
    return (rec["scene"], rec["top"], rec["game"], rec["gs"], rec["task"], rec["paused"])


def merge(recs):
    n = sum(r["n"] for r in recs)
    us = sum(r["total_us"] for r in recs)
    w = lambda key: sum(r[key] * r["n"] for r in recs) / n / 1000.0
    latn = sum(r["lat_n"] for r in recs)
    return {"n": n, "seconds": us / 1e6, "mean": us / n / 1000.0 if n else 0.0, "p50": w("p50_us"), "p95": w("p95_us"), "p99": w("p99_us"), "fps": n * 1e6 / us if us else 0.0,
            "max": max(r["max_us"] for r in recs) / 1000.0, "first": recs[0]["first_frame"], "last": recs[-1]["last_frame"],
            "lat": (sum(r["lat_p50_us"] * r["lat_n"] for r in recs) / latn / 1000.0) if latn else None}


def build_windows(run, window_seconds, keep_suspect, window_frames=0, warm=0):
    """-> (windows by class, pooled short scenes by class, counts of what was left out). window_frames > 0 (the core session): a window is
    complete at that many frame intervals whatever the rate (one 300-frame record: 10 s at 30 fps, 5 s at 60), else at window_seconds."""
    intervals = run.mark_intervals()
    limit = max((r["n"] for r in run.records if r["reason"] == "window"), default=300)
    classes = [run.classify(r, intervals) for r in run.records]
    windows, pooled = defaultdict(list), defaultdict(list)
    dropped = defaultdict(lambda: {"compile": 0, "suspect": 0, "warm": 0})
    group = []
    # warm-up (the night's protocol, docs in night.py): a stretch is the run of records with one class and one identity; its first `warm` full records are
    # dropped when it has more than `warm` of them (a stretch of one or two records is all there is: kept, flagged short)
    stretch_len, stretch_at = {}, {}
    if warm:
        key, no, ordinal = None, -1, 0
        for i, (rec, cls) in enumerate(zip(run.records, classes)):
            k = (cls, identity(rec))
            if k != key:
                key, no, ordinal = k, no + 1, 0
            stretch_at[i] = (no, ordinal)
            if rec["reason"] == "window" and rec["n"] >= 0.9 * limit:
                stretch_len[no] = stretch_len.get(no, 0) + 1
                ordinal += 1

    cls_of_group = None
    for i, (rec, cls) in enumerate(zip(run.records, classes)):
        bad = run.tainted(rec, keep_suspect)
        full = rec["reason"] == "window" and rec["n"] >= 0.9 * limit
        if warm and full and not bad:
            no, ordinal = stretch_at[i]
            if ordinal < warm and stretch_len.get(no, 0) > warm:
                dropped[cls]["warm"] += 1
                group, cls_of_group = [], None
                continue
        contiguous = group and rec["first_frame"] == group[-1]["last_frame"] and identity(rec) == identity(group[-1]) and cls == cls_of_group
        if group and not contiguous:
            group, cls_of_group = [], None
        if full and not bad:
            group.append(rec)
            cls_of_group = cls
            if (sum(r["n"] for r in group) >= window_frames * 0.9) if window_frames else (sum(r["total_us"] for r in group) / 1e6 >= window_seconds * 0.99):
                windows[cls].append(merge(group))
                group, cls_of_group = [], None
        else:
            group, cls_of_group = [], None
            if bad:
                dropped[cls][bad] += 1
        if not bad:
            pooled[cls].append(rec)
    return windows, pooled, dropped


def summarise(windows, pooled):
    """class -> {type 'W'|'S', n windows, seconds, p50, p95, p99, fps}"""
    out = OrderedDict()
    for cls in sorted(set(windows) | set(pooled)):
        ws = windows.get(cls, [])
        if ws:
            out[cls] = {"type": "W", "windows": len(ws), "seconds": sum(w["seconds"] for w in ws),
                        **{k: statistics.median(w[k] for w in ws) for k in ("mean", "p50", "p95", "p99", "fps")},
                        "lat": statistics.median(w["lat"] for w in ws if w["lat"] is not None) if any(w["lat"] is not None for w in ws) else None}
        elif pooled.get(cls) and sum(r["total_us"] for r in pooled[cls]) / 1e6 >= SHORT_MIN_SECONDS:
            m = merge(pooled[cls])
            out[cls] = {"type": "S", "windows": 0, "seconds": m["seconds"], "mean": m["mean"], "p50": m["p50"], "p95": m["p95"], "p99": m["p99"], "fps": m["fps"], "lat": m["lat"]}
    return out


# ---- the table -----------------------------------------------------------------------------------------------------------------------

CLASS_ORDER = ["menu-main", "menu-other", "stadium-select", "load", "walkout", "nis", "play", "goal:nis", "goal:replay", "goal:play-gs2", "replay", "mega-save", "pipe-walkout", "pipe-play",
               "pipe:nis", "pipe:replay", "lava-walkout", "lava-play", "lava:nis", "lava:replay", "rep:nis", "rep:replay", "pause"]


def class_key(c):
    return (CLASS_ORDER.index(c) if c in CLASS_ORDER else len(CLASS_ORDER), c)


def load_plan():
    p = HERE / "plan.json"
    return json.loads(p.read_text()) if p.exists() else {"runs": [], "refs": []}


def find_logs(root):
    root = Path(root)
    found = OrderedDict()
    if root.is_file():
        found[root.stem] = root
        return found
    for sub in sorted(p for p in root.iterdir() if p.is_dir()):
        if (sub / "log.txt").exists():
            found[sub.name] = sub / "log.txt"
    for f in sorted(root.glob("*.log")):
        found.setdefault(f.stem, f)
    return found


def order_runs(ids, plan):
    seq = [r["id"] for r in plan.get("runs", [])]
    return sorted(ids, key=lambda i: (seq.index(i) if i in seq else 10 ** 6, i))


def analyse(root, refs=None, window=10.0, keep_suspect=False, only=None, floor_ms=None, floor_fps=None, all_scenes=False, window_frames=0, warm=0):
    plan = load_plan()
    roles = {r["id"]: r for r in plan["runs"]}
    logs = find_logs(root)
    if only:
        logs = OrderedDict((k, v) for k, v in logs.items() if k in only)
    runs = OrderedDict()
    for rid in order_runs(list(logs), plan):
        runs[rid] = Run.parse(rid, Path(logs[rid]).read_text(errors="replace"))
    refs = refs or [r for r in plan.get("refs", [])]
    plan_roles = roles
    stats, dropped = OrderedDict(), {}
    for rid, run in runs.items():
        w, p, d = build_windows(run, window, keep_suspect, window_frames, warm)
        stats[rid] = summarise(w, p)
        allowed = ROUTE_CLASSES.get(plan_roles.get(rid, {}).get("route"))
        if allowed and not all_scenes:
            stats[rid] = OrderedDict((c, v) for c, v in stats[rid].items() if c in allowed)
        if plan_roles.get(rid, {}).get("role") == "census":
            stats[rid] = OrderedDict()   # (the census slows the game: its frame times are void, its log lines are the result)
        dropped[rid] = d
    # route of each run decides which scene rows it can appear in; refs are per scene: the reference runs that have the scene
    table = OrderedDict()
    classes = sorted({c for s in stats.values() for c in s}, key=class_key)
    for cls in classes:
        have = [rid for rid in refs if rid in stats and cls in stats[rid]]
        noise, base = {}, {}
        for key, _ in METRICS:
            vals = [stats[rid][cls][key] for rid in have]
            if vals:
                base[key] = sum(vals) / len(vals)
            if len(vals) >= 2:
                floor = (floor_fps if key == "fps" else floor_ms) if (floor_fps if key == "fps" else floor_ms) is not None else DEFAULT_FLOORS[key]
                noise[key] = max(2.0 * (max(vals) - min(vals)), floor)
        table[cls] = {"refs": have, "base": base, "noise": noise, "rows": OrderedDict()}
        for rid in runs:
            if cls in stats[rid]:
                s = stats[rid][cls]
                row = {"type": s["type"], "windows": s["windows"], "seconds": s["seconds"], "lat": s["lat"]}
                for key, _ in METRICS:
                    v = s[key]
                    d = v - base[key] if key in base else None
                    verdict = ""
                    if d is not None and key in noise and rid not in have and abs(d) > noise[key]:
                        better = d > 0 if key == "fps" else d < 0
                        verdict = "*" if better else "!"
                    row[key], row[key + "_d"], row[key + "_v"] = v, d, verdict
                table[cls]["rows"][rid] = row
    return plan, runs, stats, dropped, table, refs


def fmt_delta(d, verdict, digits=2):
    if d is None:
        return "   -"
    return "%+.*f%s" % (digits, d, verdict or " ")


def print_table(plan, runs, stats, dropped, table, refs, out=sys.stdout):
    roles = {r["id"]: r for r in plan["runs"]}
    p = lambda *a: print(*a, file=out)
    p("Frame time per scene per run. Deltas are against the mean of the reference runs (%s); noise = 2 x their spread; '*' better, '!' worse than the noise." % ", ".join(refs or ["none"]))
    p("win = steady 10 s windows used (~ = a short scene pooled over the run). ms are frame intervals, fps = frames / time.")
    for cls, sec in table.items():
        n = sec["noise"]
        noise_text = ("noise  p50 %.2f ms  p95 %.2f  p99 %.2f  fps %.2f" % (n["p50"], n["p95"], n["p99"], n["fps"])) if len(n) == 4 else "noise unknown (%d reference run(s) with this scene)" % len(sec["refs"])
        p("")
        p("== %s    %s" % (cls, noise_text))
        p("%-10s %-5s %4s  %8s %8s  %8s %8s  %8s %8s  %7s %7s   %s" % ("run", "ver", "win", "p50 ms", "delta", "p95 ms", "delta", "p99 ms", "delta", "fps", "delta", "lat p50"))
        for rid, row in sec["rows"].items():
            ver = roles.get(rid, {}).get("version", "?")
            win = ("%d" % row["windows"]) if row["type"] == "W" else "~"
            tag = "ref" if rid in sec["refs"] else ""
            p("%-10s %-5s %4s  %8.2f %8s  %8.2f %8s  %8.2f %8s  %7.2f %7s   %s%s" % (
                rid, ver, win, row["p50"], fmt_delta(row["p50_d"], row["p50_v"]) if not tag else "     ref", row["p95"],
                fmt_delta(row["p95_d"], row["p95_v"]) if not tag else "     ref", row["p99"], fmt_delta(row["p99_d"], row["p99_v"]) if not tag else "     ref",
                row["fps"], fmt_delta(row["fps_d"], row["fps_v"]) if not tag else "    ref",
                ("%.1f ms" % row["lat"]) if row["lat"] else "-", ""))
    # ---- what was left out and how each run went ---------------------------------------------------------------------------------------
    p("")
    p("== runs     (windows left out because they hold a shader compile: c = compile line, s = counter rise between two lines)")
    p("%-10s %-5s %-14s %-9s %6s %6s %5s %5s %s" % ("run", "ver", "clocks MHz", "frames", "c", "s", "flips", "under", "notes"))
    for rid, run in runs.items():
        d = dropped[rid]
        c = sum(v["compile"] for v in d.values())
        s = sum(v["suspect"] for v in d.values())
        frames = run.records[-1]["last_frame"] if run.records else 0
        clocks = "%d/%d/%d" % run.clocks if run.clocks else "?"
        under = run.vita_frames[-1][3] - (run.vita_frames[0][3] if run.vita_frames else 0) if run.vita_frames else 0
        notes = []
        if run.problems:
            notes.append("PROBLEM: " + "; ".join(run.problems[:2]))
        if not run.records:
            notes.append("no [frame-timing] lines: logging build? MSC_FRAME_TIMING=300?")
        if roles.get(rid, {}).get("role") == "census":
            notes.append("census: frame times void, read its [fgcensus] and [stadium-census] lines")
        if run.records and not run.counters:
            notes.append("no shader counter lines: compile exclusion by compile lines only")
        if run.fps_summary:
            sm = run.fps_summary
            notes.append("frames at 30/60: %d/%d" % (sm[3], sm[4]))
        p("%-10s %-5s %-14s %-9d %6d %6d %5d %5d %s" % (rid, roles.get(rid, {}).get("version", "?"), clocks, frames, c, s, len(run.auto_flips), under, " | ".join(notes)))
    clocks = {run.clocks for run in runs.values() if run.clocks}
    if len(clocks) > 1:
        p("WARNING: the clocks differ between runs (%s): the comparison is not at the same clocks." % ", ".join(sorted("%d/%d/%d" % c for c in clocks)))
    need = [rid for rid in refs if rid not in runs]
    if need:
        p("WARNING: reference run(s) missing: %s" % ", ".join(need))
    redo = []
    for rid, run in runs.items():
        if roles.get(rid, {}).get("role") in ("ab", "ref", "version", "cref", "core") and roles.get(rid, {}).get("route") in EXPECT and run.records:
            expect = EXPECT[roles[rid]["route"]]
            miss = [c for c in expect if c not in stats[rid]]
            if miss:
                redo.append("%s lacks %s" % (rid, ", ".join(miss)))
    if redo:
        p("RERUN candidates (no usable window for a scene): " + "; ".join(redo))


# the scenes a route must give a usable window for, or the run is a rerun candidate (and a "failed route step" in ISSUES.md)
EXPECT = {"tour": ("menu-main", "stadium-select", "walkout", "play"), "mega": ("mega-save",), "pipe": ("pipe-play",), "replay": ("rep:replay",),
          "ctour": ("menu-main", "stadium-select", "walkout", "play"), "cpipe": ("pipe-play",), "clava": ("lava-play",), "crep": ("rep:replay",),
          "ntour": ("menu-main", "stadium-select", "walkout", "play"), "npipe": ("pipe-play",), "nlava": ("lava-play",)}


# ---- the core session: extra columns per scene, stability per run, the before/after summary ---------------------------------------------

def _median(values):
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else None


def scene_extras(run, keep_suspect=False):
    """scene class -> late %, picture size, GPU busy, CPU lanes, hitches, shader compiles: from the records of the class (compile-free records for the
    timing columns) and the [lane]/[rt]/[hitch] lines printed after them."""
    intervals = run.mark_intervals()
    classes = [run.classify(r, intervals) for r in run.records]
    out = defaultdict(lambda: {"n": 0, "pics": 0, "late": 0, "sizes": [], "lanes": [], "gpu": [], "hitch": [], "compiles": 0, "scenes": []})
    for i, (rec, cls) in enumerate(zip(run.records, classes)):
        e = out[cls]
        if run.tainted(rec, keep_suspect) == "compile":
            e["compiles"] += 1
            continue
        e["n"] += rec["n"]
        if i < len(run.record_sizes):
            e["sizes"].append(run.record_sizes[i])
    for idx, values in run.lanes:
        if 0 <= idx < len(classes):
            out[classes[idx]]["lanes"].append(values)
    for idx, interval, pics, shown in run.display_win:
        if 0 <= idx < len(classes):     # late: a picture shown for more refreshes than the display interval asks (a 30 fps picture held 3 or more)
            out[classes[idx]]["pics"] += pics
            out[classes[idx]]["late"] += sum(c for k, c in enumerate(shown) if k > interval)
    for idx, sc in run.rt_scenes:
        if 0 <= idx < len(classes):
            out[classes[idx]]["scenes"].append(sc)
    for idx, p50, p95 in run.rt_gpu:
        if 0 <= idx < len(classes):
            out[classes[idx]]["gpu"].append(p50)
    for idx, gap, cause in run.hitches:
        if 0 <= idx < len(classes):
            out[classes[idx]]["hitch"].append((gap, cause))
    result = {}
    for cls, e in out.items():
        sizes = e["sizes"]
        size = max(set(sizes), key=sizes.count) if sizes else None
        lane = {k: _median(v.get(k) for v in e["lanes"]) for k in ("game", "send", "fe", "be", "gpu", "dc")}
        result[cls] = {"late": 100.0 * e["late"] / e["pics"] if e["pics"] else None, "size": size, "smallest": min(sizes, key=lambda s: s[0] * s[1]) if sizes else None,
                       "gpu_busy": _median(e["gpu"]), "scenes": _median(e["scenes"]), "lane": lane, "hitches": len(e["hitch"]),
                       "hitch_causes": dict((c, sum(1 for _, x in e["hitch"] if x == c)) for c in sorted({c for _, c in e["hitch"]})),
                       "compile_records": e["compiles"]}
    return result


PLAY_CLASSES = ("walkout", "play", "goal:nis", "goal:replay", "mega-save", "pipe-walkout", "pipe-play", "pipe:nis", "pipe:replay", "lava-walkout", "lava-play",
                "lava:nis", "lava:replay", "rep:nis", "rep:replay")


def stability(run):
    """crash / hang / heap / shader compiles during play / clean exit, from the log alone (session.sh adds the wait status and the dump files)."""
    extras = scene_extras(run, keep_suspect=True)
    compiles = sum(extras[c]["compile_records"] for c in extras if c in PLAY_CLASSES)
    h = run.heap
    block = h["block"] or 0
    fill = max(h["extent"], h["peak"]) / block if block else None
    return {"clean_exit": run.done and not run.problems, "done": run.done, "problems": list(run.problems), "heap_peak_kib": h["peak"], "heap_extent_kib": h["extent"],
            "heap_block_kib": block, "heap_fill": fill, "play_compile_records": compiles, "compile_frames": len(run.compile_frames)}


HEAP_WARN = 0.90          # extent or peak at 90 % of the C heap block: "heap near its limit"


def fmt_size(s):
    return "%dx%d" % s if s else "-"


def fmt_num(v, f="%.1f"):
    return f % v if v is not None else "-"


def core_runs(plan):
    """The core session's runs by version: {'1.1': [...], '1.3': [...], '1.3p': [...]} (performance runs only, no looks)."""
    out = defaultdict(list)
    for r in plan.get("runs", []):
        if r.get("tier") == 0 and r.get("role") in ("cref", "core"):
            out[r["version"]].append(r["id"])
    return out


def core_summary(root, out=sys.stdout, keep_suspect=False):
    """The before/after table of the core session: per scene 1.1 against 1.3 (current defaults, the mean of the cref runs) and 1.3 proposed
    against current, with noise from the reference runs (start, middle, end). Returns the rows for issues.py."""
    plan = load_plan()
    by_version = core_runs(plan)
    refs = [r["id"] for r in plan.get("runs", []) if r.get("tier") == 0 and r.get("role") == "cref"]
    only = set(sum(by_version.values(), []))
    plan_, runs, stats, dropped, table, _ = analyse(root, refs, keep_suspect=keep_suspect, only=only, window_frames=300)
    extras = {rid: scene_extras(run, keep_suspect) for rid, run in runs.items()}
    p = lambda *a: print(*a, file=out)
    rows = []
    for cls, sec in table.items():
        def agg(version):
            ids = [rid for rid in by_version.get(version, []) if rid in sec["rows"]]
            if not ids:
                return None
            pick = lambda k: statistics.mean(sec["rows"][i][k] for i in ids)
            ex = [extras[i].get(cls, {}) for i in ids]
            sizes = [e.get("size") for e in ex if e.get("size")]
            lanes = {k: _median(e.get("lane", {}).get(k) for e in ex) for k in ("game", "send", "fe", "be", "gpu")}
            return {"runs": ids, "fps": pick("fps"), "p50": pick("p50"), "p95": pick("p95"), "p99": pick("p99"),
                    "late": _median(e.get("late") for e in ex), "size": max(set(sizes), key=sizes.count) if sizes else None,
                    "gpu_busy": _median(e.get("gpu_busy") for e in ex), "lane": lanes, "hitches": sum(e.get("hitches", 0) for e in ex)}
        cur, old, prop = agg("1.3"), agg("1.1"), agg("1.3p")
        noise = sec["noise"].get("fps")
        row = {"scene": cls, "1.1": old, "1.3": cur, "1.3p": prop, "noise_fps": noise, "refs": len(sec["refs"])}
        for name, other in (("vs11", old), ("vsprop", prop)):
            verdict = ""
            if cur and other:
                d = cur["fps"] - other["fps"] if name == "vs11" else other["fps"] - cur["fps"]   # (positive: 1.3 faster than 1.1 / proposed faster than current)
                lim = noise if noise is not None else max(1.0, 0.05 * cur["fps"])
                verdict = "better" if d > lim else ("WORSE" if d < -lim else "same")
                row[name] = (d, verdict, noise is None)
        rows.append(row)
    p("== Core session: before / after (fps = median of the scene's 300-frame windows; size = the picture size most of the windows had; noise = 2 x the spread of the 1.3 reference runs)")
    p("%-15s | %-22s | %-22s | %-30s | %-22s | %s" % ("scene", "1.1 fps / size", "1.3 fps / size", "change (1.3 vs 1.1)", "1.3 proposed fps / size", "proposed vs current"))
    for row in rows:
        def cell(a):
            return ("%5.1f / %s" % (a["fps"], fmt_size(a["size"]))) if a else "-"
        def change(key, base):
            if key not in row:
                return "-"
            d, verdict, guessed = row[key]
            pct = (100.0 * d / base["fps"]) if base and base["fps"] else 0.0
            return "%+.1f fps (%+.0f%%) %s%s" % (d, pct, verdict, " (noise ~)" if guessed else "")
        p("%-15s | %-22s | %-22s | %-30s | %-22s | %s" % (row["scene"], cell(row["1.1"]), cell(row["1.3"]), change("vs11", row["1.1"]), cell(row["1.3p"]), change("vsprop", row["1.3"])))
    p("")
    p("== Core session: per scene, every version (p95/p99 ms frame time, late = pictures shown longer than the display interval ([vgx-vita] display, 600-picture windows), GPU = [rt] GPU busy p50, lanes = [lane] game/send/fe/be/gpu ms a picture, hitches = [hitch] gaps of 50 ms or more)")
    p("%-15s %-5s %6s %7s %7s %6s %-9s %6s  %-30s %s" % ("scene", "ver", "fps", "p95", "p99", "late%", "size", "GPU", "lanes game/send/fe/be/gpu", "hitch"))
    for row in rows:
        for v in ("1.1", "1.3", "1.3p"):
            a = row[v]
            if not a:
                continue
            ln = a["lane"]
            p("%-15s %-5s %6.1f %7.1f %7.1f %6s %-9s %6s  %-30s %d" % (row["scene"], v, a["fps"], a["p95"], a["p99"], fmt_num(a["late"]), fmt_size(a["size"]), fmt_num(a["gpu_busy"]),
                                                                    "/".join(fmt_num(ln[k]) for k in ("game", "send", "fe", "be", "gpu")), a["hitches"]))
    p("")
    p("== Core session: stability per run (clean = the route ended and no fatal line; heap = C heap peak in use / extent reached / block, KiB; compiles = records in play scenes holding a shader compile)")
    p("%-14s %-5s %-6s %-26s %-9s %s" % ("run", "ver", "clean", "heap peak/extent/block", "compiles", "notes"))
    roles = {r["id"]: r for r in plan.get("runs", [])}
    for rid, run in runs.items():
        s = stability(run)
        notes = []
        if s["heap_fill"] is not None and s["heap_fill"] >= HEAP_WARN:
            notes.append("HEAP %.0f%% of the block" % (100 * s["heap_fill"]))
        if s["problems"]:
            notes.append("PROBLEM: " + "; ".join(s["problems"][:2]))
        if not s["done"]:
            notes.append("route did not end")
        p("%-14s %-5s %-6s %-26s %-9d %s" % (rid, roles.get(rid, {}).get("version", "?"), "yes" if s["clean_exit"] else "NO",
                                            "%d/%d/%d" % (s["heap_peak_kib"], s["heap_extent_kib"], s["heap_block_kib"]), s["play_compile_records"], " | ".join(notes)))
    return rows


def night_stats(root, warm=2, only=None):
    """The overnight session's data per run: {run id: {"role", "route", "version", "meta", "run", "stats" (scene class -> mean/p50/p95/p99/fps of the live windows), "extras"
    (late %, picture size, GPU busy, scenes a frame, CPU lanes with draw calls, hitches, compiles), "dropped" (windows left out: compile / suspect / warm-up)}}.
    Only the runs plan.json marks as the night's (a `night` entry); the first `warm` records of every scene stretch are the warm-up (night.py)."""
    plan = load_plan()
    roles = {r["id"]: r for r in plan.get("runs", []) if "night" in r}
    out = OrderedDict()
    for rid, path in find_logs(root).items():
        if rid not in roles or (only and rid not in only):
            continue
        r = roles[rid]
        run = Run.parse(rid, Path(path).read_text(errors="replace"))
        w, p, d = build_windows(run, 10.0, False, 300, warm)
        stats = summarise(w, p)
        allowed = ROUTE_CLASSES.get(r["route"])
        if allowed:
            stats = OrderedDict((c, v) for c, v in stats.items() if c in allowed)
        out[rid] = {"role": r["role"], "route": r["route"], "version": r["version"], "meta": r["night"], "env": r.get("env", {}), "run": run, "stats": stats,
                    "extras": scene_extras(run), "dropped": d}
    return out


def baseline(data, ids, cls, key="fps", floor=None):
    """(mean, noise, n) of one scene's metric over the baseline runs `ids` that have the scene. noise = 2 x the spread (max - min) of the runs, never below the floor
    (0.3 fps / 0.5 ms) nor 2 % of the mean; with one run only the floor decides (n = 1: the caller says the noise is a guess)."""
    vals = [data[i]["stats"][cls][key] for i in ids if i in data and cls in data[i]["stats"]]
    if not vals:
        return None
    mean = sum(vals) / len(vals)
    fl = floor if floor is not None else (0.3 if key == "fps" else 0.5)
    spread = 2.0 * (max(vals) - min(vals)) if len(vals) > 1 else 0.0
    return mean, max(spread, fl, 0.02 * mean if key == "fps" else 0.0), len(vals)


def write_csv(path, table):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["scene", "run", "type", "windows", "seconds", "p50_ms", "p50_delta", "p50_noise", "p95_ms", "p95_delta", "p95_noise", "p99_ms", "p99_delta", "p99_noise", "fps", "fps_delta", "fps_noise", "verdicts"])
        for cls, sec in table.items():
            for rid, row in sec["rows"].items():
                w.writerow([cls, rid, row["type"], row["windows"], "%.1f" % row["seconds"]] + sum(
                    ([("%.3f" % row[k]), "" if row[k + "_d"] is None else "%.3f" % row[k + "_d"], "" if k not in sec["noise"] else "%.3f" % sec["noise"][k]] for k, _ in METRICS), [])
                    + ["".join("%s%s" % (k, row[k + "_v"]) for k, _ in METRICS if row[k + "_v"])])


# ---- LAN pair -------------------------------------------------------------------------------------------------------------------------

def lan_report(host_log, join_log, out=sys.stdout):
    p = lambda *a: print(*a, file=out)
    bad = re.compile(r"DESYNC|sync error|queue overflow|malformed|connection lost", re.I)
    for name, path in (("host", host_log), ("join", join_log)):
        text = Path(path).read_text(errors="replace")
        start = max([i for i, l in enumerate(text.splitlines()) if RE_BUILD.match(l)] or [0])
        lines = text.splitlines()[start:]
        mp = [l for l in lines if l.startswith("[mp-lan]")]
        delay = [l for l in mp if "input delay" in l]
        brief = [l for l in lines if l.startswith("[vita] frame ") and ("delay" in l or "rtt" in l)]
        problems = [l for l in mp if bad.search(l)]
        run = Run.parse(name, text)
        windows, pooled, _ = build_windows(run, 10.0, False)
        s = summarise(windows, pooled)
        p("== %s: %d [mp-lan] lines, %d problem line(s)" % (name, len(mp), len(problems)))
        for l in delay[:3]:
            p("   " + l[:150])
        if brief:
            p("   last link brief: " + brief[-1][:150])
        for l in problems[:5]:
            p("   PROBLEM " + l[:150])
        if "play" in s:
            q = s["play"]
            p("   play: %d window(s)  p50 %.2f ms  p95 %.2f  p99 %.2f  fps %.2f%s" % (q["windows"], q["p50"], q["p95"], q["p99"], q["fps"], ("  input delay p50 %.1f ms" % q["lat"]) if q["lat"] else ""))
        else:
            p("   no steady play window (the match did not start, or ran under 10 s)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("session", nargs="?", help="folder with <run id>/log.txt")
    ap.add_argument("--refs", help="the two reference runs, comma separated (default: the plan's)")
    ap.add_argument("--window", type=float, default=10.0, help="steady window length in seconds")
    ap.add_argument("--keep-suspect", action="store_true", help="keep windows whose compile counter rose between two lines but have no compile line of their own")
    ap.add_argument("--floor-ms", type=float, help="least noise assumed for the ms columns (default p50 0.25, p95 0.5, p99 1.0)")
    ap.add_argument("--floor-fps", type=float, help="least noise assumed for fps (default 0.3)")
    ap.add_argument("--all-scenes", action="store_true", help="also list the scenes a run's route does not report (title, menus, loading, play by match state)")
    ap.add_argument("--runs", help="only these runs, comma separated")
    ap.add_argument("--csv")
    ap.add_argument("--json")
    ap.add_argument("--lan", nargs=2, metavar=("HOST_LOG", "JOIN_LOG"))
    ap.add_argument("--header", action="store_true", help="print the [run] header lines that differ from the first reference run (did the switch take effect?)")
    ap.add_argument("--core", action="store_true", help="the core session's before/after summary, per-scene extras and stability (matrix.py tier 0)")
    args = ap.parse_args()
    if args.lan:
        lan_report(*args.lan)
        return 0
    if args.core:
        if not args.session:
            ap.error("a session folder is needed")
        core_summary(args.session, keep_suspect=args.keep_suspect)
        return 0
    if not args.session:
        ap.error("a session folder is needed")
    plan, runs, stats, dropped, table, refs = analyse(args.session, args.refs.split(",") if args.refs else None, args.window, args.keep_suspect,
                                                       args.runs.split(",") if args.runs else None, args.floor_ms, args.floor_fps, args.all_scenes)
    if not runs:
        print("no logs found in " + args.session, file=sys.stderr)
        return 2
    print_table(plan, runs, stats, dropped, table, refs)
    if args.header and refs and refs[0] in runs:
        base = set(runs[refs[0]].header)
        print("\n== [run] header lines that differ from %s" % refs[0])
        for rid, run in runs.items():
            diff = [l for l in run.header if l not in base and not l.startswith("first frame")]
            if rid != refs[0] and diff:
                print("%s:" % rid)
                for l in diff:
                    print("   " + l[:200])
    if args.csv:
        write_csv(args.csv, table)
    if args.json:
        Path(args.json).write_text(json.dumps(table, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
