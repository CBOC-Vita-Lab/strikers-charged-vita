#!/usr/bin/env python3
"""Write tools/console-1.2/fastboot/: the fast-boot variants of the console routes and their env files (test only).

    python3 tools/gen_fb_routes.py                   # write routes/fb-*.txt and env/fb-*.txt
    python3 tools/gen_fb_routes.py --check           # exit 1 when a file differs
    python3 tools/gen_fb_routes.py --env-dir DIR     # also copy env/fb-*.txt to DIR (the emulator's env folder)

MSC_BOOT_MATCH (native/platform/harness/src/np_harness.cpp; a route's `bootmatch <spec> [launch keys]` line sets it) starts the match from the title screen, so a fast-boot route is the old route
from its `wait ingame == 1` line on: no title, no main menu, no captain / side / stadium screens. The match settings the old route made with its
menus or its `launch` line move into the route's `bootmatch <stadium>:<captainA>:<captainB> <launch keys>` line.
Run: /home/claude/s13/tools/emu.sh <tag> <vpk> env/fb-<name>.txt <seconds> routes/fb-<name>.txt
The env files carry the same logging as the console-1.2 reference runs (tour.txt of /home/claude/work/v3k/env) and MSC_VITA_RENDER=320x224 for the emulator.
"""
import argparse
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
R12 = HERE / "console-1.2" / "routes"
OUT = HERE / "console-1.2" / "fastboot"

# name: (source route, scenario text, stadium for routes without a launch line, extra opts)
SCENARIOS = [
    ("tour", R12 / "tour.txt", "Vice (stadium 13, the menus' default stadium), Donkey Kong v Waluigi, the game's default options: the tour's walk-out, 62 s of play, a goal sequence", None),
    ("dump", R12 / "worstgpu.txt", "The Dump (stadium 4), the worst-case stadium for the GPU: walk-out, play, every goal's celebration and replay", None),
    ("lava", R12 / "clava.txt", "Lava Pit (stadium 7) with the infinite power-up cheat: walk-out and 20 s of play", None),
    ("pipe", R12 / "cpipe.txt", "Pipeline Central (stadium 8): walk-out and 20 s of play", None),
    ("crep", R12 / "crep.txt", "Vice, two minutes of a match of goals, each with its celebration and replay (the core session's crep route)", None),
    ("replay", R12 / "replay.txt", "Vice, a match of goals, each with its celebration and replay", None),
    ("mega", REPO / "native/tools/routes/mega_strike_route.txt", "Vice (the menus' default stadium): play until the player's captain has taken a Mega Strike", None),
    # scenes30: the whole Mega Strike as the console sees it: the captain's launch cut-scene (MegastrikeIntro), the Mega scene, the result (MegastrikeEnd), a goal's celebration and replay when
    # it scores, back to play; marks @begin / @end mega-sequence around it for [head]'s mega / mega-cut classes and the [heldgov] lines
    ("mega-full", REPO / "native/tools/routes/mega_strike_route.txt", "Vice: play until the player's captain has taken a Mega Strike, then 40 s more (launch, Mega scene, result, celebration and replay if it scores)", {"after_meter": 1200}),
]

# Goal routes (wave 11, route16): name: (source route for the bootmatch line, stadium label, mark prefix). The human side scores with the test-only route command `score`
# (np_harness.cpp: the ball is kicked into the other net with the game's own calls, so the game's own goal path, cut-scene, celebration and replay run), inside the first
# minute of play, and the route plays on through the replay and into the next kick-off. `score` is refused (logged) in LAN, online and recorded games.
GOAL_SCENARIOS = [
    ("vice-goal", R12 / "replay.txt", "Vice (stadium 13)", "rep", "rep-sequence"),
    ("dump-goal", R12 / "worstgpu.txt", "The Dump (stadium 4)", "st", "st-goal-sequence"),
    ("lava-goal", R12 / "clava.txt", "Lava Pit (stadium 7)", "lava", "lava-goal-sequence"),
]
GOAL_BODY = """wait ingame == 1 4000
log @begin {m}-walkout
try wait gs == 1 4000
log @end {m}-walkout
log STATE kick-off
try wait gs == 5 1200
log STATE open play
log @begin {m}-play
hold 18 0 71 12
hold 16 0 66 -29
hold 5 200 64 -34
hold 23 0 64 -34
try wait gs == 5 1200
score 0
log STATE score requested
try wait gs == 2 600
log @begin {g}
try wait gs == 1 3600
log @end {g}
try wait gs == 5 1200
hold 5 100 70 -15
hold 9 0 70 -15
hold 21 0 -72 0
hold 14 0 -30 66
hold 5 200 7 -72
log @end {m}-play
idle 60
log stadium route done
exit
"""


def build_goal(name, src, label, mark, seq):
    lines = src.read_text().splitlines()
    idx = next(i for i, l in enumerate(lines) if l.startswith("wait ingame == 1"))
    boot, opts = "vice:3:6", ""
    if idx > 0 and LAUNCH.match(lines[idx - 1]):
        kv = dict(t.split("=", 1) for t in LAUNCH.match(lines[idx - 1]).group(1).split())
        assert kv.pop("mode") == "friendly" and kv.pop("human") == "0"
        boot = "%s:%s:%s" % (kv.pop("stadium"), kv.pop("home"), kv.pop("away"))
        opts = " ".join("%s=%s" % (k, v) for k, v in kv.items())
    what = "%s: kick-off, a few seconds of play, then the test-only `score 0` command kicks the ball into the net: goal, celebration and replay in the first minute, then play on" % label
    route = ["# fast boot (test only): %s" % what,
             "# Generated by tools/gen_fb_routes.py (GOAL_SCENARIOS; bootmatch line from %s); edit the generator, not this file." % src.relative_to(REPO),
             "# `score` exists only in route files and is refused (logged, the route goes on) in LAN, online and recorded games and outside open play.",
             ("bootmatch %s %s" % (boot, opts)).rstrip()]
    body = GOAL_BODY.format(m=mark, g=seq).splitlines()
    env = ENV_HEAD.format(what=what, name=name, src=src.name, boot=boot, opts="")
    return "\n".join(route + body) + "\n", env


LAUNCH = re.compile(r"^launch\s+(.*)$")
ENV_HEAD = """# fast boot (test only): {what}
# Run: emu.sh <tag> <vpk> <this file> <seconds> <routes/fb-{name}.txt>   (the route is the console-1.2 route {src}, from its `wait ingame == 1` line on)
# ---- the fast boot is switched on by the route's first line (`bootmatch {boot} ...`): in the emulator the Vita build's env file did not deliver MSC_BOOT_MATCH ----
# ---- route and logging: as the console-1.2 reference runs ----
MSC_INPUT_SCRIPT=ux0:data/mscharged-perf-test/fb-{name}.txt
MSC_FRAME_TIMING=300
MSC_LAT=1
MSC_WATCH=1
MSC_WATCH_EVERY=600
MSC_VITA_RENDER=320x224
"""


def build(name, src, what, extra=None):
    lines = src.read_text().splitlines()
    idx = next(i for i, l in enumerate(lines) if l.startswith("wait ingame == 1"))
    boot, opts = "vice:3:6", ""
    if idx > 0 and LAUNCH.match(lines[idx - 1]):
        kv = dict(t.split("=", 1) for t in LAUNCH.match(lines[idx - 1]).group(1).split())
        assert kv.pop("mode") == "friendly" and kv.pop("human") == "0"
        boot = "%s:%s:%s" % (kv.pop("stadium"), kv.pop("home"), kv.pop("away"))
        opts = " ".join("%s=%s" % (k, v) for k, v in kv.items())
    body = lines[idx:]
    if extra and extra.get("after_meter"):   # (scenes30 mega-full: the route waits through the whole Mega Strike sequence after the meter instead of 240 frames)
        k = body.index("idle 240")
        assert body[k - 1] == "log Mega Strike meter stopped" and body[k + 1] == "log Mega Strike done", "mega route changed: update gen_fb_routes.py"
        body = body[:k] + ["log @begin mega-sequence", "idle %d" % extra["after_meter"], "log @end mega-sequence"] + body[k + 1:]
    route = ["# fast boot (test only): %s" % what,
             "# Generated by tools/gen_fb_routes.py from %s (its lines from `wait ingame == 1` on); edit the generator, not this file." % src.relative_to(REPO),
             "# The bootmatch line below sets MSC_BOOT_MATCH (and _OPTS) for the harness: the match is started at the title screen, so no title, menu or launch lines.",
             "# The menu scene marks (@begin menu-main, stadium-select) are gone by design; @begin/@end walkout and play marks are as in the source route."]
    route.append(("bootmatch %s %s" % (boot, opts)).rstrip())
    route += body
    env = ENV_HEAD.format(what=what, name=name, src=src.name, boot=boot, opts=("MSC_BOOT_MATCH_OPTS=%s\n" % opts) if opts else "")
    return "\n".join(route) + "\n", env


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--env-dir")
    a = ap.parse_args()
    bad = 0
    items = [(n, build(n, s_, w, x)) for n, s_, w, x in SCENARIOS] + [(n, build_goal(n, s_, l, m, q)) for n, s_, l, m, q in GOAL_SCENARIOS]
    for name, (route, env) in items:
        files = {OUT / "routes" / ("fb-%s.txt" % name): route, OUT / "env" / ("fb-%s.txt" % name): env}
        if a.env_dir:
            files[Path(a.env_dir) / ("fb-%s.txt" % name)] = env
        for p, text in files.items():
            if a.check and "env-dir" not in str(p) and a.env_dir is None:
                if not p.exists() or p.read_text() != text:
                    print("differs: %s" % p)
                    bad = 1
            else:
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(text)
    return bad


sys.exit(main())
