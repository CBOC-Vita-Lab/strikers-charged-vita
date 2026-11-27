#!/usr/bin/env python3
"""Synthetic session logs in the console's format, for testing analyse.py (and for seeing what a finished session looks like).

    python3 synth.py <out dir>        # writes <out dir>/<run id>/log.txt for a small session: d1, d2, msaa0, fxn, r11, ...

The lines are the ones the game prints (np_harness.cpp EmitTiming, vgx_host_vita.cpp, np_vita_platform.cpp, np_fps.cpp); the frame times are
generated, the percentiles in the [frame-timing] lines are computed from them by the same rules as NpFrameTiming::Window (300-frame windows,
a short window at every change of scene identity, the changing frame itself not counted).
"""
import json
import random
import sys
from pathlib import Path

# (name, frames, scene, game state, task state, mean ms at 30 fps, mark): the tour, as the game reports it
TOUR = [
    ("title", 240, 0, -1, 4, 33.4, None),
    ("menu-main", 660, 1, -1, 4, 33.4, "menu-main"),
    ("menus", 400, 2, -1, 4, 33.4, None),
    ("stadium-select", 660, 5, -1, 4, 33.4, "stadium-select"),
    ("load", 120, 7, -1, 0x200000, 50.0, None),
    ("walkout", 640, 9, 0, 16, 33.4, "walkout"),
    ("kickoff", 90, 9, 1, 2, 33.4, None),
    ("play", 1980, 9, 5, 2, 33.4, "play"),
    ("goal", 140, 9, 2, 16, 33.4, "goal-sequence"),
    ("replay", 260, 9, 2, 8, 33.4, "goal-sequence"),
    ("kickoff2", 60, 9, 1, 2, 33.4, None),
]
MEGA = [
    ("title", 240, 0, -1, 4, 33.4, None),
    ("menus", 900, 1, -1, 4, 33.4, None),
    ("mega-save", 1500, 9, 5, 2, 33.4, "mega-save"),
]
# The routes of 1.3: a stadium (marks pipe-*), and goal sequences (mark rep-sequence)
PIPE = [
    ("title", 240, 0, -1, 4, 33.4, None),
    ("load", 120, 7, -1, 0x200000, 50.0, None),
    ("walkout", 640, 9, 0, 16, 150.0, "pipe-walkout"),
    ("kickoff", 90, 9, 1, 2, 150.0, None),
    ("play", 660, 9, 5, 2, 150.0, "pipe-play"),
]
LAVA = [(n, f, s, g, k, ms, m.replace("pipe", "lava") if m else m) for n, f, s, g, k, ms, m in [
    ("title", 240, 0, -1, 4, 33.4, None), ("load", 120, 7, -1, 0x200000, 50.0, None), ("walkout", 640, 9, 0, 16, 33.4, "pipe-walkout"),
    ("kickoff", 90, 9, 1, 2, 33.4, None), ("play", 660, 9, 5, 2, 33.4, "pipe-play")]]
REPLAY = [
    ("title", 240, 0, -1, 4, 33.4, None),
    ("kickoff", 90, 9, 1, 2, 33.4, None),
    ("play", 400, 9, 5, 2, 33.4, None),
    ("goal", 140, 9, 2, 16, 33.4, "rep-sequence"),
    ("replay", 260, 9, 2, 8, 33.4, "rep-sequence"),
    ("kickoff2", 60, 9, 1, 2, 33.4, None),
]


# the overnight routes (night.py): longer open play, so that a stretch has a warm-up and live windows
NPIPE = [(n, f * (2 if n == "play" else 1) + (300 if n == "play" else 0), sc, gs, k, ms, m) for n, f, sc, gs, k, ms, m in PIPE]
NLAVA = [(n, f * (2 if n == "play" else 1) + (300 if n == "play" else 0), sc, gs, k, ms, m) for n, f, sc, gs, k, ms, m in LAVA]
NTOUR = [(n, f + (600 if n == "play" else 0), sc, gs, k, ms, m) for n, f, sc, gs, k, ms, m in TOUR]


class Window:
    """NpFrameTiming::Window in Python."""
    def __init__(self, limit=300):
        self.limit, self.anchored, self.count = limit, False, 0
        self.last_frame = self.last_us = 0
        self.seg = None
        self.intervals, self.first_frame, self.start_us = [], 0, 0

    def anchor(self, frame, now, seg):
        self.anchored, self.last_frame, self.last_us, self.seg = True, frame, now, seg

    def flush(self, reason):
        if not self.intervals:
            return None
        s = sorted(self.intervals)
        pick = lambda p: s[(len(s) * p + 99) // 100 - 1]
        rec = dict(reason=reason, first_frame=self.first_frame, last_frame=self.last_frame, n=len(s), start_us=self.start_us, end_us=self.last_us,
                   total_us=sum(s), min_us=s[0], p50_us=pick(50), p95_us=pick(95), p99_us=pick(99), max_us=s[-1],
                   over_33333=sum(1 for x in s if x > 33333), over_16667=sum(1 for x in s if x > 16667), seg=self.seg)
        self.intervals = []
        return rec

    def add(self, frame, now, seg):
        out = []
        if not self.anchored:
            self.anchor(frame, now, seg)
            return out
        if seg != self.seg:
            r = self.flush("identity")
            if r:
                out.append(r)
            self.anchor(frame, now, seg)
            return out
        if not self.intervals:
            self.first_frame, self.start_us = self.last_frame, self.last_us
        self.intervals.append(now - self.last_us)
        self.anchor(frame, now, seg)
        self.last_frame = frame
        if len(self.intervals) == self.limit:
            out.append(self.flush("window"))
        return out


def gen_run(rid, route, ms_by_name=None, seed=1, hz60=(), compiles=(), build="synthetic", clocks="ARM 444 MHz, GPU 222 MHz, bus 222 MHz, GPU xbar 166 MHz",
            extra_header=(), jitter=0.04, spike=0.002, counters_from=0, flips=3, core=None):
    """Text of one run's log. ms_by_name: segment name -> mean interval in ms (default the segment's own); hz60: segment names shown at 60 fps
    (mean interval halved unless given); compiles: frame numbers with a shader compile in them. core: dict of the core session's extra lines
    (gpu ms, size (w, h) from frame 'size_at', heap (in use, extent, block) KiB, hitches n): a [lane] and an [rt] line after every record."""
    rng = random.Random(seed)
    segs = {"pipe": PIPE, "replay": REPLAY, "mega": MEGA, "cpipe": PIPE, "clava": LAVA, "crep": REPLAY, "npipe": NPIPE, "nlava": NLAVA, "ntour": NTOUR}.get(route, TOUR)
    ms_by_name = dict(ms_by_name or {})
    out = ["[vita] ===== Mario Strikers Charged, PS Vita native port, build %s =====" % build,
           "[run] first frame: build %s (test); clocks %s" % (build, clocks),
           "[run] fps=auto, thresholds synthetic", "[run] dynres: on", "[run] picture=synthetic", "[run] gfx: synthetic", "[run] settings: synthetic", "[run] controls: synthetic"]
    out += ["[run] " + h for h in extra_header]
    out.append("[frame-timing-config] clock=real-monotonic hook=game-frame-accounting metric=accounting-pacing-interval gpu_complete=0 window=300 max_records=1024 percentile=nearest-rank")
    win = Window()
    frame, now = 0, 1_000_000
    events = []      # (frame, text) lines that belong between frame-timing lines
    compiles = sorted(compiles)
    compile_count = 0
    last_counter_frame = 0
    lines = []
    marks_open = {}
    for si, (name, frames, scene, gs, task, base_ms, mark) in enumerate(segs):
        ms = ms_by_name.get(name, base_ms / (2 if name in hz60 and name not in ms_by_name else 1))
        seg = (scene, 0x1000 + scene, 0x2000, gs, task, 0)
        if mark and mark not in marks_open.values() and mark != "goal-sequence":
            lines.append("[script] f=%d @begin %s" % (frame, mark))
            marks_open[name] = mark
        if mark == "goal-sequence" and "goal-sequence" not in marks_open.values():
            lines.append("[script] f=%d @begin goal-sequence" % frame)
            marks_open[name] = mark
        for _ in range(frames):
            frame += 1
            step = rng.gauss(ms, ms * jitter)
            if rng.random() < spike:
                step += ms * rng.uniform(1.0, 3.0)
            now += int(max(step, 1.0) * 1000)
            for f in compiles:
                if f == frame:
                    now += 140_000
                    compile_count += 6
                    lines.append("[vgx-vita] present %d came 140 ms after the previous one: +3 vertex shaders, +3 fragment shaders, +6 programs, +0 textures" % frame)
            if core and core.get("size_at") == frame:
                lines.append("[vgx] size change to %dx%d: 3.0 ms on the CPU (targets 2.0), next presents 33.4 33.4 33.4 33.4 ms apart" % core["size"])
            for rec in win.add(frame, now, seg):
                lines.append(fmt_record(rec))
                if core:
                    g = core.get("gpu", 20.0)
                    lines.append("[lane] 300 pics 10.02s 29.9/s game 14.2 send 3.1 fe 12.4 be 7.3/9.8 gpu %.1f cores 71/63/58/4 dc %d soft 9120 static 210 th 3" % (g, core.get("dc", 612)))
                    lines.append("[rt] 300 frames: scenes 9.0/frame (display 1.0), depth not loaded 1.0 (by the whole-clear rule 1.0), not stored 1.0, hints 1.0 given 1.0 used; "
                                 "depth MB/frame loaded 0.10 stored 0.10, not loaded 0.10, not stored 0.10, mask-clear quads saved 0.0 | CPU scene change 0.100 ms/frame (end 0.010 begin 0.010 set-up 0.010; "
                                 "frame p50 0.100 p95 0.200, max scene 0.300 ms) | draw that begins a scene 10.0 us (x9.0/frame), other draws 5.0 us | GPU (300 of the last frames timed) busy p50 %.2f p95 %.2f ms, "
                                 "first submit to last done p50 20.00 p95 25.00 ms, last submit to done p50 5.00 p95 6.00 ms; scenes timed 2700 untimed 0 already done when looked at 0, pending 0" % (g, g * 1.2))
                    for _ in range(core.get("hitches", 0)):
                        lines.append("[hitch] gap=62.0 shader swap=1.0 scene=9 frame %d" % frame)
            if frame % 600 == 0:
                lines.append("[vgx-vita] present %d: 120 draw calls in the last buffer, 40 vertex + 60 fragment shaders and 90 programs so far, 300 textures, 9000 KiB of texels" % frame)
                lines.append("[vgx-vita] shader work so far: %d compiles, %d links, %d ms in all, progress bar shown 0 times" % (compile_count + counters_from, compile_count, 100 * compile_count))
            if core and frame % 600 == 0:
                late = 30 if ms > 34.5 else 3
                lines.append("[vgx-vita] display: interval 2 vblank(s), the last 600 pictures were shown for 0/1/2/3/4+ vblanks: 0/0/%d/%d/0 (all so far %d: 0/0/0/0/0; synthetic)" % (600 - late, late, frame))
            if frame % 1800 == 0:
                lines.append("[vita] frame %d: 30.0 fps, game thread 20.00 ms CPU/frame, audio underruns 0, dropped 0, C heap 1000 KiB in use (1200 KiB taken from the system)" % frame)
        if mark and (si + 1 == len(segs) or segs[si + 1][6] != mark):
            lines.append("[script] f=%d @end %s" % (frame, mark))
    for rec in win.add(frame + 1, now + 33000, ("end",)) + [win.flush("end")]:
        if rec:
            lines.append(fmt_record(rec))
    for i in range(flips):
        lines.append("[fps] auto: 30 -> 60 at t=%.3f s, frame %d, task state 0x2 (synthetic)" % (10.0 + i, 300 + 100 * i))
    lines.append("[fps] summary: mode=4 frames=%d (game 0, 30 %d, 60 %d, unlocked 0, cut-scene lock 0) frame lengths in refreshes 1/2/3/4+ = 0/0/0/0 mean 33.40 ms; pictures stayed 0/1/2/3/4/5+ refreshes = 0/0/0/0/0/0; switches=%d" % (frame, frame, 0, flips))
    if core and core.get("lock60"):
        for scene, over in core["lock60"]:
            lines.append("[lock60] %s ended: 900 measured frames, %d over the 60 budget (%.1f%%), steps down 3 up 2, rungs 0..4, level now 2" % (scene, int(9 * over), over))
    if core and core.get("heap"):
        used, extent, block = core["heap"]
        lines.append("[mem] C heap: block %d KiB (taken whole at start-up); in use now %d KiB, most ever in use %d KiB (sampled every 30 frames), extent reached %d KiB" % (block, used, used, extent))
    if not (core and core.get("hang")):
        lines.append("[plat] script exit at frame %d, exiting" % frame)
    return "\n".join(out + lines) + "\n"


def fmt_record(r):
    seg = r["seg"]
    if seg == ("end",):
        scene, top, game, gs, task, paused = -1, 0, 0, -1, 0, 0
    else:
        scene, top, game, gs, task, paused = seg
    return ("[frame-timing] reason=%s first_frame=%d last_frame=%d n=%d start_us=%d end_us=%d total_us=%d min_us=%d p50_us=%d p95_us=%d p99_us=%d max_us=%d "
            "over_33333=%d over_16667=%d scene=%d top=%08x game=%08x gs=%d task=%d paused=%d gpu_complete=0 lat_n=%d lat_p50_us=%d lat_p95_us=%d" % (
                r["reason"], r["first_frame"], r["last_frame"], r["n"], r["start_us"], r["end_us"], r["total_us"], r["min_us"], r["p50_us"], r["p95_us"], r["p99_us"], r["max_us"],
                r["over_33333"], r["over_16667"], scene, top, game, gs, task, paused, r["n"], 90000, 110000))


def session(out_dir):
    """A small session with known answers: d1 and d2 are the same within noise; msaa0 costs 2 ms in play; fxn saves 3 ms in cut-scenes and
    replays; timing0 has a shader compile in the middle of play; r11 runs at 60 fps in the menu and is 4 ms slower in play."""
    out = Path(out_dir)
    spec = {
        "d1": dict(route="tour", seed=11),
        "d2": dict(route="tour", seed=12),
        "msaa0": dict(route="tour", seed=13, ms_by_name={"play": 35.4, "menu-main": 36.0}),
        "fxn": dict(route="tour", seed=14, ms_by_name={"walkout": 30.4, "goal": 30.4, "replay": 30.4}),
        "timing0": dict(route="tour", seed=15, compiles=[3700]),
        "r11": dict(route="tour", seed=16, hz60=("menu-main", "stadium-select", "menus"), ms_by_name={"play": 37.4}, build="synthetic v1.1"),
        "megad1": dict(route="mega", seed=21),
        "megad2": dict(route="mega", seed=22),
        "r10m": dict(route="mega", seed=23, ms_by_name={"mega-save": 41.0}, build="synthetic v1.0"),
    }
    for rid, kw in spec.items():
        (out / rid).mkdir(parents=True, exist_ok=True)
        (out / rid / "log.txt").write_text(gen_run(rid, **kw))
    return spec


def core_session(out_dir):
    """A small core session with known answers: 1.3 current play at 30 fps against 1.1 at 25 (better); the proposed Pipeline Central 4 ms slower
    (a regression); cur-mega2 with its C heap at 95 % of the block; v11-rep never ends (a hang); cur-pipe1 with a compile in play; sizes from a size line."""
    out = Path(out_dir)
    big = dict(size=(960, 544), size_at=500, gpu=20.0)
    spec = {
        "cur-tour1": dict(route="ctour", seed=41, core=big), "cur-tour2": dict(route="ctour", seed=42, core=big), "cur-tour3": dict(route="ctour", seed=43, core=big),
        "cur-pipe1": dict(route="cpipe", seed=44, ms_by_name={"walkout": 40.0, "play": 40.0}, compiles=[1400], core=dict(big, gpu=36.0)),
        "cur-pipe2": dict(route="cpipe", seed=45, ms_by_name={"walkout": 40.0, "play": 40.0}, core=dict(big, gpu=36.0)),
        "cur-pipe3": dict(route="cpipe", seed=46, ms_by_name={"walkout": 40.0, "play": 40.0}, core=dict(big, gpu=36.0)),
        "cur-lava1": dict(route="clava", seed=47, core=big), "cur-lava2": dict(route="clava", seed=48, core=big),
        "cur-mega1": dict(route="mega", seed=49, core=dict(big, heap=(60000, 70000, 98304))),
        "cur-mega2": dict(route="mega", seed=50, core=dict(big, heap=(90000, 93400, 98304))),
        "cur-rep1": dict(route="crep", seed=51, core=big), "cur-rep2": dict(route="crep", seed=52, core=big),
        "prop-tour": dict(route="ctour", seed=53, core=big),
        "prop-pipe": dict(route="cpipe", seed=54, ms_by_name={"walkout": 44.0, "play": 44.0}, core=dict(big, gpu=40.0)),
        "prop-lava": dict(route="clava", seed=55, core=big), "prop-mega": dict(route="mega", seed=56, core=big),
        "v11-tour": dict(route="ctour", seed=57, ms_by_name={"play": 40.0, "menu-main": 55.0}, build="synthetic v1.1", core=dict(size=(640, 448), size_at=-1, gpu=30.0)),
        "v11-pipe": dict(route="cpipe", seed=58, ms_by_name={"walkout": 150.0, "play": 150.0}, build="synthetic v1.1", core=dict(gpu=140.0)),
        "v11-lava": dict(route="clava", seed=59, ms_by_name={"play": 36.0}, build="synthetic v1.1", core=dict(gpu=30.0)),
        "v11-mega": dict(route="mega", seed=60, ms_by_name={"mega-save": 66.0}, build="synthetic v1.1", core=dict(gpu=60.0)),
        "v11-rep": dict(route="crep", seed=61, build="synthetic v1.1", core=dict(gpu=30.0, hang=True)),
    }
    for rid, kw in spec.items():
        (out / rid).mkdir(parents=True, exist_ok=True)
        (out / rid / "log.txt").write_text(gen_run(rid, **kw))
        (out / rid / "status.txt").write_text("stalled\n" if kw.get("core", {}).get("hang") else "done\n")
    return spec


def gen_stab(rid, matches, ended, crash=False, climb=False, expect_exit=True, build="synthetic", stadiums=(3, 1, 8, 15, 14, 7, 9, 11, 3, 1, 8, 15), tag="stab"):
    """The log of a stability route: `matches` begun, `ended` of them ended, a crash after the last one begun when crash (no script exit), the C heap climbing when climb."""
    out = ["[vita] ===== Mario Strikers Charged, PS Vita native port, build %s =====" % build,
           "[run] first frame: build %s (test); clocks ARM 444 MHz, GPU 222 MHz, bus 222 MHz, GPU xbar 166 MHz" % build, "[run] fps=auto, thresholds synthetic"]
    frame, heap = 100, 20000
    for i in range(matches):
        s = stadiums[i % len(stadiums)]
        name = "%s-%02d-s%d" % (tag, i + 1, s)
        out.append("[script] f=%d @begin %s" % (frame, name))
        for k in range(3):
            frame += 600
            heap += 4000 if climb else (6000 if k == 0 else -3000)
            out.append("[mem] C heap: block 65536 KiB (taken whole at start-up); in use now %d KiB, most ever in use %d KiB (sampled every 30 frames), extent reached %d KiB" % (heap, heap + 2000, heap + 4000))
        if i < ended:
            out.append("[script] f=%d @end %s" % (frame, name))
            heap -= 2000 if not climb else 0
    if crash:
        out.append("[vgx] frame %d: vertex arena could not grow" % frame)
    elif expect_exit:
        out.append("[plat] script exit at frame %d, exiting" % frame)
    return "\n".join(out) + "\n"


def night_session(out_dir):
    """A small night with known answers (the ids are night.py's). Baselines: Pipeline Central 25 fps, Lava Pit 30, tour 30; LOCKED 60 at 59.5 on Pipeline Central; 1.1 at 6.7.
    n-fxtight 27.8 (ADOPT), n-fxgov 23.3 (LEAVE OFF), n-memfree2 same (NO GAIN), n-gpubusy0 22.7 with the default on (KEEP ON), n-memfree0 26.3 (FLIP OFF),
    n-viewlayers +0.7 fps over a 0.3 noise (marginal), n-hsr without its proof line (INCONCLUSIVE), n-finclamp2 never ends (BROKEN), n-texfull picture and faster.
    Stability: s-stab ends all twelve matches, s-intro crashes in its fourth stadium, o-stab (1.1) stops in its first match with a dump."""
    out = Path(out_dir)
    big = dict(size=(960, 544), size_at=500, gpu=20.0)
    def put(rid, text, status="done", dumps=()):
        (out / rid).mkdir(parents=True, exist_ok=True)
        (out / rid / "log.txt").write_text(text)
        (out / rid / "status.txt").write_text(status + "\n")
        if dumps:
            (out / rid / "dumps.txt").write_text("\n".join(dumps) + "\n")
    seed = [100]
    def perf(rid, route, ms, status="done", core=None, **kw):
        seed[0] += 1
        names = {"npipe": ("walkout", "play"), "nlava": ("walkout", "play"), "ntour": ("play",), "mega": ("mega-save",), "crep": ("goal", "replay")}[route]
        put(rid, gen_run(rid, route, ms_by_name={n: ms for n in names}, seed=seed[0], core=core or dict(big), **kw), status)
    # baselines (1.3 defaults) with a little spread between runs
    for rid, ms in (("b-npipe-1", 40.0), ("b-npipe-d3", 40.3), ("b-npipe-d5", 39.8), ("b-npipe-2", 40.2)):
        perf(rid, "npipe", ms, core=dict(big, gpu=36.0))
    for rid, ms in (("b-nlava-1", 33.4), ("b-nlava-2", 33.5)):
        perf(rid, "nlava", ms)
    for rid, ms in (("b-ntour-1", 33.4), ("b-ntour-d4", 33.5), ("b-ntour-2", 33.3)):
        perf(rid, "ntour", ms)
    perf("b-mega-1", "mega", 33.4); perf("b-mega-2", "mega", 33.5); perf("b-crep-1", "crep", 33.4); perf("b-crep-2", "crep", 33.5)
    # LOCKED 60
    perf("l-npipe-1", "npipe", 16.8, core=dict(big, gpu=14.0, lock60=[("play", 1.5)])); perf("l-npipe-2", "npipe", 16.9, core=dict(big, gpu=14.0))
    perf("l-ntour-1", "ntour", 16.7); perf("l-nlava-1", "nlava", 19.0); perf("l-mega-1", "mega", 16.7); perf("l-crep-1", "crep", 18.0)
    # 1.1
    for rid, route, ms in (("o-npipe-1", "npipe", 150.0), ("o-nlava-1", "nlava", 36.0), ("o-ntour-1", "ntour", 40.0), ("o-mega-1", "mega", 66.0), ("o-crep-1", "crep", 45.0)):
        perf(rid, route, ms, build="synthetic v1.1", core=dict(gpu=60.0))
    # decisions
    perf("n-fxtight", "npipe", 36.0, core=dict(big, gpu=30.0)); perf("n-fxgov", "npipe", 43.0); perf("n-memfree2", "npipe", 40.1)
    perf("n-gpubusy0", "npipe", 44.0); perf("n-memfree0", "npipe", 38.0); perf("n-texfull", "npipe", 37.0)
    perf("n-viewlayers", "nlava", 32.7)
    perf("n-hsr", "nlava", 33.4)
    perf("n-finclamp2", "nlava", 33.4, status="stalled", core=dict(big, hang=True))
    # stability
    put("s-stab", gen_stab("s-stab", 12, 12))
    put("s-intro", gen_stab("s-intro", 4, 3, crash=True, tag="stab", stadiums=(0, 2, 4, 5, 6, 10, 12, 13)), status="stalled")
    put("s-end", gen_stab("s-end", 3, 3, tag="end", stadiums=(3, 8, 15)))
    put("o-stab", gen_stab("o-stab", 1, 0, crash=True, build="synthetic v1.1"), status="stalled", dumps=["psp2core-2026_11_12-02_14_00-eboot.bin.psp2dmp"])
    # looks: real PNGs, the switch's picture differs from the baseline's in the middle
    import looks
    def png(level, patch=None):
        w, h = 64, 48
        rows = bytearray(bytes([level, level, level]) * (w * h))
        if patch:
            for y in range(10, 30):
                for x in range(10, 30):
                    rows[(y * w + x) * 3:(y * w + x) * 3 + 3] = bytes([patch] * 3)
        return looks.png_bytes(w, h, bytes(rows))
    for rid, patch in (("b-look", None), ("n-look-menustill", 200), ("n-look-hudnative", None)):
        for n in (700, 800, 900):
            d = out / rid / "shots"
            d.mkdir(parents=True, exist_ok=True)
            d.joinpath("shot_%06d.png" % n).write_bytes(png(90 + n // 100, patch))
        (out / rid / "log.txt").write_text(gen_run(rid, "ntour", seed=7))
        (out / rid / "status.txt").write_text("done\n")
    (out / "night-progress.txt").write_text("elapsed_min,phase,id,status\n3,p0,b-npipe-1,done\n360,p2,n-texfull,done\n")
    (out / "skipped.txt").write_text("n-quadfast,p2,deadline\nn-sbatch1,p2,deadline\n")
    (out / "not-in-this-build.txt").write_text("n-passdown\tMSC_VGX_PASSDOWN(agent/passes2)\n")


if __name__ == "__main__":
    target = sys.argv[1] if len(sys.argv) > 1 else "synthetic-session"
    session(target)
    print("wrote %s" % target)
