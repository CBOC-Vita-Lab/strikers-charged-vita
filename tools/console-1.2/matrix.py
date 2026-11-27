"""The console A/B matrix for 1.3 (the folder keeps its 1.2 name): one entry per run, in the order a session runs them. gen_kit.py turns it into env/*.txt and plan.json.

A run is one launch of the test app with one np-player.txt (the env file) and one route. Everything a run does not list is the
default of the candidate (version "1.3"). Since 1.2 the defaults are the best experience, not the old behaviour: Prioritize Players, anti-aliasing as a ladder rung,
the shadow-quad cut, lazy depth of field, sharp distant textures, the Galaxy timing items, bounded slip and the 256-vertex rigid merge are
all ON in the build (vita_plan.cpp, "quality and timing switches"). So the A/B runs turn each item OFF: the line in an env file is the
value that switches the item off (=0, or the 1.1 value where 0 means something else: MSC_DOF=1 is the game's own capture, MSC_VGX_SOFTXFORM=128
the 1.1 mesh limit). A run that beats the d1/d2 noise by being slower than d1 says the default is wrong and should go back. Order keeps the shader cache warm: the 1.2 defaults run first (the package carries the donor shader pack, and the first run
fills ux0:data/mscharged-perf-test's own cache for the stock route), then switches that add no shader variants, then the one that
adds the effects pass to cut-scenes (after the Mega Save run has compiled that pass), the looks, and last the older versions, which
have their own cache keys, each run twice in a row only where a cold start is expected (never: a compile inside a window drops that
window, see README.md "Pass rules").

1.3: the questions 1.3 opened come first, in order of expected value, so a session cut short has answered the most. Only the 1.3 default in the
same route is a reference: a Pipeline Central or replay run is compared with its own route's reference pair (pipe1/pipe2, rep1/rep2), never with the tour.

Minutes are estimates for a stock 444 MHz Vita (see ROUTE_MIN); the session driver writes the real times and `gen_kit.py --plan`
prints the estimate again with the measured routes.
"""
from pathlib import Path

import gen_online as _GO   # online24: the routes, their STATE lines and the run table of unit ON (routes/on-*.txt come from gen_routes.py)


# Estimated console minutes per run, launch to pulled log. Measured values replace these (session-times.csv).
ROUTE_MIN = {"worstgpu": 5.5, "tour": 4.9, "mega": 2.8, "look": 2.0, "pipe": 5.0, "ctrl": 3.5, "replay": 6.0,   # (pipe: the reported 6-7 fps scene; replay: up to five goal sequences)
             # the core session's routes (gen_routes.py): host harness 6,028 / 2,197 / 2,219 / 5,720 presented frames, at 30 fps plus about a minute of start-up and pull
             "ctour": 4.4, "cpipe": 2.3, "clava": 2.3, "crep": 4.2, "settings": 1.0,
             # the overnight session's routes (night.py): longer open play for a warm-up and live windows; the stability routes measure nothing
             "ntour": 4.8, "npipe": 2.8, "nlava": 2.8, "cstab": 24.0, "cintro": 13.0, "cend": 8.0,
             # r16: the goal-scenario fast-boot routes (route16, `score` command): walk-out, kick-off, a goal with celebration and replay, play on, idle 60 s (estimate, no console figure)
             "gsvice": 3.5, "gsdump": 3.5, "gslava": 3.5,
             # scenes30: the whole Mega Strike at Vice (fb-mega-full: play to the captain's Mega Strike, then 40 s through launch, Mega scene, result, celebration and replay); fast boot only (estimate)
             "megafull": 3.0,
             # r17 (kit20): Sand Tomb (fb-sand, copy of fb-lava with stadium 15), and the settings-screen routes driven by the route's own pad and finger presses
             "sand": 1.5, "pause": 3.0, "live19": 3.0, "live19c": 3.0, "live19r": 3.0, "live19l": 3.0,
             # online24: the online runs of the real service (gen_online.py): menus and Online Central by pad presses, one console, no opponent (ESTIMATES; the two queue runs hold 60 s)
             **{_k: _v[3] for _k, _v in _GO.ROUTES.items()}}
# Version 1.1 is slower where the console logs of 1.0/1.1 said so (menus 18-22 fps, Pipeline Central 6-7 fps, replays 20-25): minutes added to its runs.
SLOW_11 = {"ctour": 1.0, "cpipe": 2.0, "clava": 0.7, "crep": 0.8, "look": 0.3, "mega": 0.5, "ntour": 1.0, "npipe": 2.5, "nlava": 1.0, "cstab": 0.0, "cintro": 0.0, "cend": 0.0}
SWAP_MIN = 1.5            # pushing another version's eboot.bin and shader pack, reading them back, no reboot
SETUP_MIN = 2.0           # check, stage routes, first launch prompts
LAN_LEG_MIN = 4.0         # one two-Vita match leg (both Vitas run at once)
CORE_BUDGET_MIN = 125.0   # the core session (tier 0), setup and four swaps included; see core_blocks() for the minutes of each block
BUDGET_MIN = 374.0 + CORE_BUDGET_MIN + 8.0   # (r17 kit20: +8 for the two overlapped-step LAN legs; wave 10 r15: stall10 +15 for discard0, rehearse6, loadhold1; bg30b +10 for bg30pair, bg30pairdump; bg30 +15 for bg30half, bg30proof, bg30dump; gt10 +3 for bpfull1; warm10: +5 for warmspread0, the old warm-up guard; wave 9 physpar: +6 for physpose0, the kill-switch run of the pose lanes; pacing12: +5 for clockslip; pacing11: +5 for bp0, back-pressure off; wave 9: +10 for the pacing runs latdepthp1, padlate0 (wave 10: latdrain0 too), betweenold; 1.3 core session: tier 0 runs first in `all` and `full`; (release 1.3: +5 for pipemist0, the Pipeline Central mist switches off, the players' top complaint; 1.3 defaults: +10 for the two defaults-all, the current-against-proposed run; 93 for 1.2; 1.3 adds the runs of the 1.3 questions, two routes and the 1.2 regression pair: three and a half hours, or two sittings split at dmid: `all` resumes)

# Written into every run's file: what is read, what is logged. Identical in every run, references included.
COMMON = [
    ("MSC_FRAME_TIMING", "300"),      # [frame-timing] windows of 300 frames: p50/p95/p99, scene, task, game state
    ("MSC_LAT", "1"),                 # the lat_* fields of [frame-timing] and the [lat] lines (the player build has them off)
    ("MSC_WATCH", "1"),               # [nav] scene / match-state changes
    ("MSC_WATCH_EVERY", "600"),
    ("MSC_VITA_CLOCK_POLICY", "max"),     # clock17 (owner, 14 Nov: "the same as Galaxy, 500 and 222"): every run is the player's policy, a deferred 500 request (444 if refused; a PSVshellPlus profile at about 500 is left alone).
                                          # Before it the base was `fixed` (444 over a profile), which is why r11-r13 read ARM 444. The stock-console runs are clock444, clock444d (MSC_VITA_CPU_MHZ=444)
]
# The reader of MSC_VITA_SHOT takes up to 16 items, each a present number or a range "a-b" (every present of it) or "a-b/step"; the isolated
# build refuses a "/" in a value, so the shots are 16 single presents, 100 apart (3.3 s at 30 fps). The look route (host harness: walk-out
# presents 792-1380, open play 1395-2221) gives 1 menu picture, 6 of the walk-out and 9 of play.
SHOT_PRESENTS = ",".join(str(n) for n in range(700, 2201, 100))
SHOTS = [("MSC_FPS", "30"), ("MSC_VITA_SHOT", SHOT_PRESENTS), ("MSC_VITA_SHOT_SIZE", "640x448")]
# wt/scenes SCENE-AUDIT.md, section 1: what convicts or clears each candidate for Pipeline Central. The census slows the game, so these runs' frame times are void (role "census").
# MSC_VGX_PASSES names presents in open play of the pipe route (the look route's timing: open play from present 1395 at 30 fps), where the audit had 3000,6000 for a longer match.
CENSUS = {"MSC_STADIUM_CENSUS": "1", "MSC_VGX_FGCENSUS": "1", "MSC_VGX_FGCENSUS_WINDOW": "300", "MSC_VGX_BYPASSLOG": "1", "MSC_VGX_DLLIVE": "300", "MSC_VGX_GEOMCENSUS": "1",
          "MSC_VGX_TEXLOG": "1", "MSC_FX_STAT": "1", "MSC_VGX_PASSES": "1800,2300", "MSC_VGX_PASSMEMO_LOG": "1", "MSC_VIEW_LAYERS": "1", "MSC_AMORT_LOG": "1"}
# PERFORMANCE (60) as the Settings screen wrote it, in 1.2 and in the 1.3 development builds (the preset left the screen in the 1.3 prune; the kit keeps the pairs, the readers stay)
PERF12 = {"MSC_PERFORMANCE_MODE": "1", "MSC_FPS_REPLAY": "30", "MSC_DYNRES_MAX": "1.0", "MSC_VGX_MSAA": "0", "MSC_FX_LOWRES_SCENES": "nis", "MSC_FX_LOWRES_MINPX": "480000", "MSC_VITA_ANISO": "0"}
PERF13 = dict(PERF12, MSC_FX_LOWRES_MINPX="325000", MSC_FX_LOWRES="4", MSC_DYNRES_MIN60="0.70", MSC_FPS_BETWEEN="pace", MSC_VITA_EDGE_AA="0", MSC_SHADOW_EVERY="2", MSC_REPLAY_FARLOD="12")



def run(id, title, route, env=None, version="1.3", tier=1, role="ab", group="", why="", block="", settings=""):
    return dict(id=id, title=title, route=route, env=list((env or {}).items()), version=version, tier=tier, role=role, group=group, why=why, block=block, settings=settings)


# ---- fast boot (wave 10, fastboot10; test only): a route that targets a stadium / match starts the match at the title screen (the route's first line `bootmatch <spec>`
# sets MSC_BOOT_MATCH inside the game: no intro movie wait, no menus, no captain / side / stadium screens). The kit makes env-fb/<id>.txt for the runs that can use it and
# session.sh uses it when FASTBOOT=1 (or a `fastboot 1` line of the list). Not for the runs that measure menus, intros, stability, natural match ends, LAN or the Settings
# screen, nor the look runs (their pictures are numbered presents counted from the old route's start) nor the Mega Save tutorial (menus are the route).
FB_ROUTE = {"tour": "fb-tour", "worstgpu": "fb-dump", "cpipe": "fb-pipe", "clava": "fb-lava", "replay": "fb-replay", "crep": "fb-crep",
            "gsvice": "fb-vice-goal", "gsdump": "fb-dump-goal", "gslava": "fb-lava-goal", "megafull": "fb-mega-full",
            "sand": "fb-sand", "pause": "fb-pause", "live19": "fb-live19", "live19c": "fb-live19-crowd", "live19r": "fb-live19-rear", "live19l": "fb-live19-leave"}
# minutes the fast boot saves a run (ESTIMATES from the route scripts, no console figure yet): the tour loses the 20 s main-menu hold and the menus (about 2 min);
# the others only the title, main menu and the launch (about 0.8 min)
FB_SAVE_MIN = {"tour": 2.0, "worstgpu": 0.8, "cpipe": 0.8, "clava": 0.8, "replay": 0.8, "crep": 0.8, "gsvice": 0.0, "gsdump": 0.0, "gslava": 0.0, "megafull": 0.0,
               "sand": 0.0, "pause": 0.0, "live19": 0.0, "live19c": 0.0, "live19r": 0.0, "live19l": 0.0}
FB_NOT = ("menulod1", "menulod2", "menulod3", "hudnative", "lock60tour")   # runs that are about menus
MENU_KEYS = ("MSC_MENU_LOD", "MSC_MENU_STILL", "MSC_VGX_HUDNATIVE", "MSC_LANGUAGE")


FB_ROUTE_MIN = {FB_ROUTE[k]: ROUTE_MIN[k] - FB_SAVE_MIN[k] for k in FB_ROUTE}


def fastboot_route(r):
    """The fast-boot route name of a run (fb-tour, fb-dump, ...) or None when the run must keep the old route."""
    if r["route"] not in FB_ROUTE or r["role"] in ("look", "settings") or r["settings"] or r["id"] in FB_NOT or r.get("night"):
        return None
    keys = {k for k, _ in r["env"]}
    if "MSC_VITA_SHOT" in keys or keys & set(MENU_KEYS):
        return None
    return FB_ROUTE[r["route"]]


def minutes(r, fastboot=False):
    """Estimated console minutes of one run (launch to pulled log); fastboot=True: with the fast-boot route where the run has one."""
    m = ROUTE_MIN[r["route"]] + (SLOW_11.get(r["route"], 0.0) if r["version"] == "1.1" else 0.0)
    if fastboot and fastboot_route(r):
        m -= FB_SAVE_MIN[r["route"]]
    return m


# ---- the core session (tier 0): before / after with real console numbers in about two hours --------------------------------------------------
# Versions: "1.3" = the current defaults (knobs; final/strikers-13-final-current.vpk), "1.3p" = the proposed defaults (branch defaults-proposed;
# final/strikers-13-final-proposed.vpk), "1.1" = the published 1.1 source (git 5421c34) staged as the isolated logging app (README.md "Version 1.1").
# Order controls drift (heat, battery): current at the start, after the proposed block (middle) and at the end, so the proposed runs sit between two
# current blocks and the 1.1 runs between two others. Every performance run carries MSC_HITCH_MS=50 (diagnostic: a [hitch] line per gap of 50 ms or more;
# 1.1 does not know the key and ignores it). Look runs are separate launches at a locked 30 fps (a screenshot costs a frame); they are never timed.
CORE_COMMON = [("MSC_HITCH_MS", "50")]
LAVA = {"MSC_CAMERA_ZOOM": "1", "MSC_CAMERA_AUTO": "0"}      # the farthest camera, held (the most draws and fill); the tour and the stadiums keep the default 5 with Auto Zoom
SHOTS_PIPE = [("MSC_FPS", "30"), ("MSC_VITA_SHOT", "900,1150,1350,1450,1500,1550,1600,1650,1700,1750,1800,1850,1900,1950,2000,2100"), ("MSC_VITA_SHOT_SIZE", "640x448")]
SHOTS_MEGA = [("MSC_FPS", "30"), ("MSC_VITA_SHOT", ",".join(str(n) for n in range(1000, 2501, 100))), ("MSC_VITA_SHOT_SIZE", "640x448")]
SHOTS_REP = [("MSC_FPS", "30"), ("MSC_VITA_SHOT", ",".join(str(n) for n in range(1800, 4801, 200))), ("MSC_VITA_SHOT_SIZE", "640x448")]
SMALL = "864x488"         # the Quality floor (18 Nov: nothing under 864x488; was 826x468, the old 0.86 rung): where the 1-px edge line of the old viewport rounding shows
# The Settings screen, opened by the isolated app's test hook (np_vita_settings.cpp: the file np-settings-open in the private root, its words are keys,
# shot:NAME writes shots/settings-NAME.ppm); set18: its two pages (SAFE START, ONLINE), then Circle (no change: leaves without saving, the app quits). The game's own
# VITA SETTINGS menu is not reached by this hook.
SETTINGS_ES = "wait:2500 shot:es-1 rtrigger wait:800 shot:es-2 wait:500 circle"


def _core():
    out = []
    def perf(id, title, route, version, block, env=None, ref=False, why=""):
        out.append(run(id, title, route, env, version=version, tier=0, role="cref" if ref else "core", group="core", why=why, block=block))
    def look(id, title, route, version, block, shots, env=None, why=""):
        out.append(run(id, title, route, dict(shots, **(env or {})), version=version, tier=0, role="look", group="core-look", why=why, block=block))
    A, B, C, D, E = ("A: 1.3 current, start", "B: 1.3 proposed", "C: 1.3 current, middle", "D: 1.1 as published", "E: 1.3 current, end")
    ref = "Reference: the current defaults. Its spread with the other current runs of this route is the noise."
    perf("cur-tour1", "1.3 current: menus, stadium select, walk-out, play, goal (start)", "ctour", "1.3", A, ref=True, why=ref)
    perf("cur-pipe1", "1.3 current: Pipeline Central (start)", "cpipe", "1.3", A, ref=True, why=ref)
    perf("cur-lava1", "1.3 current: Lava Pit, items every 7.5 s, farthest camera (start)", "clava", "1.3", A, LAVA, ref=True, why=ref)
    perf("cur-mega1", "1.3 current: Mega Save (start)", "mega", "1.3", A, ref=True, why=ref)
    perf("cur-rep1", "1.3 current: goals, celebrations, replays (start)", "crep", "1.3", A, ref=True, why=ref)
    lk = "Pictures at a locked 30 fps (never timed): contact sheet column against 1.1 and the proposed defaults."
    look("cur-look", "Look 1.3 current: main menu, walk-out, open play (shadows, HUD, glow)", "look", "1.3", A, SHOTS, why=lk)
    look("cur-lookpipe", "Look 1.3 current: Pipeline Central mist", "cpipe", "1.3", A, SHOTS_PIPE, why=lk)
    look("cur-lookmega", "Look 1.3 current: Mega Save effects and glow", "mega", "1.3", A, SHOTS_MEGA, why=lk)
    look("cur-lookrep", "Look 1.3 current: celebrations and replays", "crep", "1.3", A, SHOTS_REP, why=lk)
    look("cur-looksmall", "Look 1.3 current at a fixed %s: 1-px edge lines, static on shadows and decals" % SMALL, "look", "1.3", A, SHOTS, {"MSC_VITA_RENDER": SMALL},
         why="The exact-viewport fix (MSC_VGX_VP_EXACT) and the FS_TRIM final clamp at a reduced odd size: no dark line on the right/bottom edge of glow and shadows, no noise on shadows or decals.")
    look("cur-lookes", "Look 1.3 current in Spanish: menus and HUD text with accents", "look", "1.3", A, SHOTS, {"MSC_LANGUAGE": "es"},
         why="MSC_LANGUAGE=es: the disc's Spanish tables. Accented letters (á é í ó ú ñ ¿ ¡) must draw, not boxes or gaps.")
    out.append(run("cur-setes", "SAFE START and ONLINE pages in Spanish (the port's own texts)", "settings", {"MSC_LANGUAGE": "es"}, version="1.3", tier=0, role="settings",
                   group="core-look", block=A, settings=SETTINGS_ES,
                   why="The isolated app's Settings hook opens the screen, takes a picture of each page and leaves with Circle (nothing saved). The port's own texts in Spanish, with accents."))
    pr = "The proposed defaults against the current blocks before and after it: decides whether the proposal ships as a unit."
    perf("prop-tour", "1.3 proposed: menus, stadium select, walk-out, play, goal", "ctour", "1.3p", B, why=pr)
    perf("prop-pipe", "1.3 proposed: Pipeline Central", "cpipe", "1.3p", B, why=pr)
    perf("prop-lava", "1.3 proposed: Lava Pit, items, farthest camera", "clava", "1.3p", B, LAVA, why=pr)
    perf("prop-mega", "1.3 proposed: Mega Save", "mega", "1.3p", B, why=pr)
    look("prop-look", "Look 1.3 proposed: main menu, walk-out, open play", "look", "1.3p", B, SHOTS, why=lk + " Glow half, shadows every 3rd frame, still menus, smaller textures.")
    look("prop-lookpipe", "Look 1.3 proposed: Pipeline Central mist (tight sprites, fill governor)", "cpipe", "1.3p", B, SHOTS_PIPE, why=lk)
    look("prop-lookmega", "Look 1.3 proposed: Mega Save (governor in Mega)", "mega", "1.3p", B, SHOTS_MEGA, why=lk)
    perf("cur-tour2", "1.3 current: tour (middle)", "ctour", "1.3", C, ref=True, why=ref)
    perf("cur-pipe2", "1.3 current: Pipeline Central (middle)", "cpipe", "1.3", C, ref=True, why=ref)
    old = "Version 1.1 as published (its own source, staged as the isolated logging app, its own shader pack): the before of before/after."
    perf("v11-tour", "1.1: menus, stadium select, walk-out, play, goal", "ctour", "1.1", D, why=old)
    perf("v11-pipe", "1.1: Pipeline Central", "cpipe", "1.1", D, why=old)
    perf("v11-lava", "1.1: Lava Pit, items, farthest camera", "clava", "1.1", D, LAVA, why=old)
    perf("v11-mega", "1.1: Mega Save", "mega", "1.1", D, why=old)
    perf("v11-rep", "1.1: goals, celebrations, replays", "crep", "1.1", D, why=old)
    look("v11-look", "Look 1.1: main menu, walk-out, open play", "look", "1.1", D, SHOTS, why=lk)
    look("v11-lookpipe", "Look 1.1: Pipeline Central mist", "cpipe", "1.1", D, SHOTS_PIPE, why=lk)
    look("v11-lookmega", "Look 1.1: Mega Save", "mega", "1.1", D, SHOTS_MEGA, why=lk)
    look("v11-lookrep", "Look 1.1: celebrations and replays", "crep", "1.1", D, SHOTS_REP, why=lk)
    look("v11-looksmall", "Look 1.1 at a fixed %s (the edge lines 1.3 fixed should show here)" % SMALL, "look", "1.1", D, SHOTS, {"MSC_VITA_RENDER": SMALL}, why=lk)
    perf("cur-tour3", "1.3 current: tour (end)", "ctour", "1.3", E, ref=True, why=ref)
    perf("cur-pipe3", "1.3 current: Pipeline Central (end)", "cpipe", "1.3", E, ref=True, why=ref)
    perf("cur-lava2", "1.3 current: Lava Pit, items, farthest camera (end)", "clava", "1.3", E, LAVA, ref=True, why=ref)
    perf("cur-mega2", "1.3 current: Mega Save (end)", "mega", "1.3", E, ref=True, why=ref)
    perf("cur-rep2", "1.3 current: goals, celebrations, replays (end)", "crep", "1.3", E, ref=True, why=ref)
    # ---- add-ons (tier 3): after everything else in `full`; the 500 MHz one needs the owner's PSVshellPlus profile (role manual: `session.sh addon` only)
    out.append(run("cap-tour", "1.3 current with the fourth core asked for (CapUnlocker): tour", "ctour", {"MSC_VITA_CORE3": "1", "MSC_VITA_CORE3_SEND": "1", "MSC_VITA_CLOCK_POLICY": "max"}, tier=3, group="addon",
                   block="F: add-ons", why="With CapUnlocker the log says `[vita] fourth core: usable` and sound, disc, log and frame send move there; without it `not available` and the run equals cur-tour*. Compare play p50 and the [lane] game column with cur-tour1..3."))
    out.append(run("cap-pipe", "1.3 current with the fourth core asked for: Pipeline Central", "cpipe", {"MSC_VITA_CORE3": "1", "MSC_VITA_CORE3_SEND": "1", "MSC_VITA_CLOCK_POLICY": "max"}, tier=3, group="addon",
                   block="F: add-ons", why="As cap-tour, on the heaviest stadium."))
    out.append(run("clk500-tour", "1.3 current at 500 MHz (PSVshellPlus profile set by hand)", "ctour", {"MSC_VITA_CLOCK_POLICY": "max"}, tier=3, role="manual", group="addon", block="F: add-ons",
                   why="Needs the owner's PSVshellPlus 500 MHz profile for CDXMP0001: the `clocks:` line must say about 498 MHz measured. Against cur-tour1..3."))
    return out


CORE = _core()


RUNS = CORE + [
    # ---- the candidate ------------------------------------------------------------------------------------------------------------------
    run("d1", "1.3 defaults (first reference)", "tour", role="ref", group="reference",
        why="Baseline; first of the two reference runs whose difference sets the noise threshold."),
    run("megad1", "1.3 defaults, Mega Save", "mega", role="ref", group="reference",
        why="Baseline for the Mega Save route; also compiles the effects pass the cut-scene run below uses."),
    run("defaults-all", "Every proposed 1.3 default on together (docs/TECHNICAL.md, 1.3 Proposed Defaults)", "tour",
        {"MSC_FX_TIGHT": "8", "MSC_FX_TIGHT_DROP": "1", "MSC_FX_GOV": "1", "MSC_FX_GOV_MEGA": "1", "MSC_FX_GLOW": "half", "MSC_SHADOW_PLAY": "3", "MSC_AMORT_60": "1",
         "MSC_SHADOW_PLANE": "1", "MSC_CROWD_CLUSTER_LAZY": "1", "MSC_VIEW_LAYERS": "1", "MSC_MENU_STILL": "1", "MSC_PAUSE_STILL": "1", "MSC_VGX_LA88": "2",
         "MSC_VGX_CMPRA": "2", "MSC_VGX_TEX16": "1", "MSC_DVD_PREOPEN": "1", "MSC_LOAD_FPS": "60", "MSC_VITA_CORE3": "1",
         "MSC_VITA_CORE3_SEND": "1", "MSC_VITA_QUEUE_WAIT": "event"}, group="defaults",
        why="The current defaults (d1) against the proposed ones in one go: menus, play, Mega Strikes and replays of the tour. p50, late frames, GPU ms and [lane] against d1 and dmid; "
            "by eye: menus settle still after about two thirds of a second, softer glow, shadows. With CapUnlocker the log must say `[vita] fourth core: usable`, without it `not available` and nothing else differs. "
            "A slower run, a fault or a broken picture stops the proposal; the single runs (fxtight, fxgov, fxglowhalf, shadowplay3, amort60, shadowplane, clusterlazy, cmpra2, lai8, tex16, loadfps60, sweepbg0) say which item."),
    run("defaults-all-pipe", "Every proposed 1.3 default on together, Pipeline Central at the default picture mode", "pipe",
        {"MSC_FX_TIGHT": "8", "MSC_FX_TIGHT_DROP": "1", "MSC_FX_GOV": "1", "MSC_FX_GOV_MEGA": "1", "MSC_FX_GLOW": "half", "MSC_SHADOW_PLAY": "3", "MSC_AMORT_60": "1",
         "MSC_SHADOW_PLANE": "1", "MSC_CROWD_CLUSTER_LAZY": "1", "MSC_VIEW_LAYERS": "1", "MSC_MENU_STILL": "1", "MSC_PAUSE_STILL": "1", "MSC_VGX_LA88": "2",
         "MSC_VGX_CMPRA": "2", "MSC_VGX_TEX16": "1", "MSC_DVD_PREOPEN": "1", "MSC_LOAD_FPS": "60", "MSC_VITA_CORE3": "1",
         "MSC_VITA_CORE3_SEND": "1", "MSC_VITA_QUEUE_WAIT": "event"}, group="defaults",
        why="Pipeline Central, the stadium the 1.3 defaults aim at, at the default picture mode (dynamic resolution, AUTO frame rate), against pipe1 and pipe2: pipe-play p50, late frames, "
            "GPU ms, [vgx] dynamic resolution steps and mean scale, [vgx] fill governor lines (the governor acts here: the mist is more than a screen of large sprites). By eye: the mist for grain. "
            "If play stays under 30, run hazecache3heavy next (the haze picture on top of the heavy profile)."),
    # ---- the questions of 1.3, in order of expected value ----------------------------------------------------------------------------------
    run("softx128", "Rigid merge limit back to 128 vertices", "tour", {"MSC_VGX_SOFTXFORM": "128"}, group="draw",
        why="Galaxy went 128 -> 32 for 40 -> 45.7 fps; Strikers raised the limit to 256 in 1.2, unmeasured. Compare the [lane] FE ms and p50 with d1. 256 cuts 4.4 % of the draw calls on the host; pixel changes are rounding."),
    run("softx64", "Rigid merge limit 64 vertices", "tour", {"MSC_VGX_SOFTXFORM": "64"}, group="draw",
        why="The next step down from softx128 (0 is rejected: Galaxy lost 30 s at 10 fps to uploads at stage start). Watch the first seconds of walk-out and play for upload hitches as well as the p50."),
    run("softx32", "Rigid merge limit 32 vertices (Galaxy's value)", "tour", {"MSC_VGX_SOFTXFORM": "32"}, group="draw", tier=2,
        why="Galaxy's measured best (FE 20.5 -> 16 ms a picture, 40 -> 45.7 fps at 32). Only if softx64 beats d1: the same watch for upload hitches at walk-out."),
    run("unishadow0", "Uniforms through glUniform again", "tour", {"MSC_VGX_UNI_SHADOW": "0"}, group="draw", tier=2,
        why="MSC_VGX_UNI_SHADOW (on): uniform blocks written into vitaGL's buffers (patch 0024, Galaxy's GXV_UNI_SHADOW). Same picture by construction; judge the back end's ms ([lane]) against d1. Slower than d1 means keep it on."),
    run("newstadium0", "Ladder keeps its level across a stadium change", "tour", {"MSC_DYNRES_NEWSTADIUM": "0"}, group="draw", tier=2,
        why="MSC_DYNRES_NEWSTADIUM (on): a new stadium starts the size ladder at the game's own size (Galaxy's GXV_AUTOQ_NEWSTAGE). The menus are GPU-heavy (about 40 ms), so the match may start low without it. Compare the [vgx] dynamic resolution lines and the first 30 s of the match's picture size with d1."),
    run("vpexact0", "Viewports halved as integers again (the 1-px line)", "tour", {"MSC_VGX_VP_EXACT": "0"}, group="picture", tier=2,
        why="MSC_VGX_VP_EXACT (on, vitaGL 0025): odd-sized viewports exact. Look for a 1-px dark line on the right or bottom edge of glow, shadows and copies when the picture is below 960x544 (the ladder's 0.86 floor). The log's [gxm-direct] viewports line says which rounding is in force."),
    run("pxscissor0", "Scissor changed for every draw again", "tour", {"MSC_VGX_PX_SCISSOR": "0"}, group="draw", tier=2,
        why="MSC_VGX_PX_SCISSOR (on): scissor changes that cannot cut anything are not made (each costs vitaGL a full-target mask draw). Compare GPU ms with d1, and check the picture: any difference (geometry past its viewport) means the console does not clip to the viewport and the switch must go off."),
    run("pxcut0", "Pixel-work cuts off", "tour", {"MSC_VGX_PX_NOOP": "0", "MSC_VGX_PX_DEADCLEAR": "0"}, group="draw", tier=2,
        why="MSC_VGX_PX_NOOP / MSC_VGX_PX_DEADCLEAR (on): no-write draws and dead depth planes of clears left out. Same picture by construction; GPU ms against d1."),
    run("blind0", "Ladder blind again when the GPU timers go silent", "tour", {"MSC_DYNRES_BLIND": "0"}, group="draw", tier=2,
        why="MSC_DYNRES_BLIND (on, the Galaxy port's r86): after 6 pictures without a timed GPU frame, late pictures whose CPU lanes fit the budget step the ladder down. First read d1's log: any 'no timed GPU frame for' line means the blind spot happens here (menus and stadium select: 17 scenes a frame, GPU ~40 ms). Then compare the [frame-timing] p50 and late frames of those stretches and the 'late pictures with the GPU timers silent' level changes."),
    run("blindpipe0", "Ladder blind again when the GPU timers go silent, Pipeline Central", "pipe", {"MSC_DYNRES_BLIND": "0"}, group="draw", tier=2,
        why="As blind0 on the heaviest stadium (mist: the GPU over its budget for long stretches), against pipe1: 'no timed GPU frame' lines, pipe-play p50, late frames, picture scale."),
    run("hold30off", "Ladder steps down on one GPU spike at 30 again", "tour", {"MSC_DYNRES_HOLD30": "0"}, group="draw", tier=2,
        why="MSC_DYNRES_HOLD30=4 (on, the Galaxy port's hold30): at 30 the mean-over-90 % step waits for 4 measured frames over 90 %. Against d1: the number of 'the GPU is over its budget' level changes a minute in open play (the host model: 0.7 a minute on one-frame spikes at 65 % load without it, none with it), the mean scale, and late frames (must not rise)."),
    run("maskkeep0", "Scissor mask redrawn at every scene again", "tour", {"MSC_VGX_VGL_MASKKEEP": "0"}, group="draw", tier=2,
        why="MSC_VGX_VGL_MASKKEEP (on, vitaGL 0026, the Galaxy port's r84 MASK_KEEP): a scissor-mask rebuild the depth surface already holds is not drawn (two full-target quads each). Same picture by construction; check it anyway by eye (any part of the picture missing or drawn past its frame means the mask did not survive and the switch must go off). Against d1: GPU busy ms ([rt]/[gpu] lines) and the '[rt] ... scissor mask rebuilds' line (drawn and kept per frame)."),
    run("memfree2", "Free-memory counters checked against the heap walk", "tour", {"MSC_VGX_VGL_MEMFREE": "2"}, group="draw", tier=1,
        why="MSC_VGX_VGL_MEMFREE (on, vitaGL 0028, the Galaxy port's free-memory fix): the free video memory the resolution ladder asks for comes from counters kept by vitaGL's allocator wrappers instead of a walk of the whole heap. =2 answers with the walked figure and compares: the '[mem] free-figure questions' line must show 0 differ (a few 'skipped' are fine) and the pool mask 0x3 or more (bit 0 video memory, bit 1 main RAM pool). Any 'free figure kept on the heap walk' line in the log means the start-up probes refused a pool (then the switch changes nothing there; send the line). Also read the [vitaGL] lines for 'MEMFREE pool' (a counted difference)."),
    run("memfree0", "Free-memory figure by heap walk again", "tour", {"MSC_VGX_VGL_MEMFREE": "0"}, group="draw", tier=2,
        why="As d1 with the old walk (MSC_VGX_VGL_MEMFREE=0). Against d1: the renderer thread's per-frame time and the back end's ms ([rt]/[frame-timing]) as the session grows: the walk costs time in proportion to the textures loaded, so the gap should open over the tour and be largest right after a stadium with many textures; the free-memory numbers in the '[vgx] mega warm' lines must agree."),
    run("gcpause1000", "Allocation-recovery wait of one second again", "tour", {"MSC_VGL_GC_PAUSE_MS": "1000"}, group="draw", tier=2,
        why="MSC_VGL_GC_PAUSE_MS (16 ms, vitaGL 0036): when every video / main-memory pool refuses an allocation vitaGL finishes the GPU, forces collector cycles and waits for memory other threads give back; patch 0005 waited one second per recursion level (a frozen picture of 1 to 4 s). =1000 is about the old wait: the OFF run. Read the '[mem] GPU memory recoveries' line at each report and its '[mem] NOTE' line: with 0 recoveries in d1 the pools never ran dry, and this run must show the same 0 (nothing to compare). If d1 shows recoveries, compare the 'stopped ... ms in all' and 'longest' figures here against d1 and look for [hitch] lines and a missing goalie or goal-scene texture at the same frames."),
    run("maskbg0", "Full-target mask quad drawn in every scene again", "tour", {"MSC_VGX_VGL_MASKBG": "0"}, group="draw", tier=2,
        why="MSC_VGX_VGL_MASKBG=1 (on, vitaGL 0029, the Galaxy port's MASK_KEEP / patch 0018): a scene that does not load its depth/stencil surface and has the scissor test off begins with the mask set and the full-target mask quad is not drawn. Against d1: GPU busy ms ([rt]/[gpu]) and the '[rt] scissor mask background' line (scenes armed per run); the picture must be the same (any part of the picture missing or drawn outside its frame, or stencil-masked effects gone, means it goes off). Quads saved per frame is the scene count that began unloaded."),
    run("maskbg1", "Mask background for the display's scene off again (value 1; plan default 3 since gpu24)", "tour", {"MSC_VGX_VGL_MASKBG": "1"}, group="draw", tier=2,
        why="The A side of the plan default MSC_VGX_VGL_MASKBG=3: the picture to the screen (the display's scene, one full-target quad every picture) also starts with the mask set (the Galaxy port's patch 0022, tried only on one Vita). Against d1: the display scene's share of GPU busy, a picture identical by eye in the menus, play and the pause still, and no black or half-drawn present."),
    run("passhalf", "Glow blur steps at half resolution", "tour", {"MSC_VGX_PASSHALF": "1"}, group="picture", tier=2,
        why="MSC_VGX_PASSHALF=1 (off here, on in the proposed defaults; the Galaxy port's GXV_PASSHALF=2): the glow chain's blur steps at half the render scale, a quarter of their pixels. Against d1: GPU busy ms in the menus and Mega Strikes ([rt]/[gpu]), and the glow by eye (slightly softer edges expected; blocky glow means it goes off)."),
    run("passdown2q0", "Glow steps of two quads drawn in two scenes again", "replay", {"MSC_VGX_PASSDOWN_TWOQUADS": "0"}, group="picture", tier=2,
        why="MSC_VGX_PASSDOWN_TWOQUADS=0 (default 1: the glow chain's steps in The Dump and Sand Tomb, drawn as two quads, are drawn straight at half scale like Vice's, 13 scenes of a celebration become 7; host: 21 to 15 scenes at The Dump frame 2290, pictures identical at 3 frames). Against the default on goal celebrations at The Dump and Sand Tomb: GPU busy ms and late frames in the celebration, the glow edge by eye."),
    run("passdown", "Glow chain steps drawn straight at the copy's size", "replay", {"MSC_VGX_PASSDOWN": "0"}, group="picture", tier=2,
        why="MSC_VGX_PASSDOWN=0 (the build default is 1): with it on each of the glow chain's six blur steps is drawn at half size straight into its destination, no copy scene: six scenes fewer in replays, celebrations and Mega Strikes. This run turns it off. Against rep1/rep2: `[rt] scenes` per frame (expect 6 more here), GPU busy ms in rep:replay and rep:nis, and the glow by eye (edges within 2-3 colour levels; a visibly blockier or brighter halo means it goes off)."),
    run("finclamp0", "Final colour clamp kept in every shader", "tour", {"MSC_VGX_FINCLAMP": "0"}, group="picture", tier=1,
        why="MSC_VGX_FINCLAMP=0 (the build default is 3: the clamp is left out where the value ranges prove it is not needed; shader pack 16 holds both forms, so no compile). FIRST THING TO LOOK AT on the console: blended shadows, decals and multisampled edges for static or wrapped colours with the default; if any, this run is the fix and the default goes back to 0. Timing: GPU busy ms against the default."),
    run("canon0", "Shader simplifier off", "tour", {"MSC_VGX_TEV_CANON": "0", "MSC_VGX_FINCLAMP": "0"}, group="picture", tier=2,
        why="MSC_VGX_TEV_CANON=0 with the clamp kept: the pack 15 shader forms. Against the default and finclamp0: picture and GPU busy ms."),
    run("discard0", "Discarded frames paced at one retrace each", "worstgpu", {"MSC_DISCARD_FAST": "0"}, group="smooth", tier=1,
        why="MSC_DISCARD_FAST=0 (default 1: the frames the game throws away at the end of loading and in the walk-out's wait are not paced at one retrace each). Against the default: the [discard] lines (frames, ms a frame, not paced) and the [hitch] gaps after [nav] match state -1 -> 0 (about 1.9 s and 1.2 s on r11)."),
    run("rehearse6", "Walk-out wait frames sent hidden (on side)", "worstgpu", {"MSC_DISCARD_REHEARSE": "6"}, group="smooth", tier=1,
        why="MSC_DISCARD_REHEARSE=6 (default 0): every 6th discarded GameBegin frame is executed hidden under the loading picture. Against the default: the [discard] lines (sent hidden), the [hidden] lines and the [hitch] gap at the first walk-out picture (shaders, targets, uploads)."),
    run("loadhold1", "First walk-out frame made hidden (on side)", "worstgpu", {"MSC_DISCARD_REHEARSE": "6", "MSC_LOADHOLD": "1"}, group="smooth", tier=1,
        why="MSC_DISCARD_REHEARSE=6 and MSC_LOADHOLD=1 (default 0 and 0): the walk-out's first frame is also executed hidden. Against the default and rehearse6: [hitch] pool/sh fields of the first presented walk-out picture."),
    run("prewarm0", "Picture programs built at first use", "tour", {"MSC_PIC_PREWARM": "0"}, group="smooth", tier=2,
        why="MSC_PIC_PREWARM=0 (default 1 builds the picture and crowd-clear programs at start-up). Against the default: `[hitch]` lines naming a shader in play."),
    run("poolkeep0", "Render targets trimmed after 600 presents", "replay", {"MSC_POOL_KEEP": "0"}, group="smooth", tier=2,
        why="MSC_POOL_KEEP=0 (default 18000 keeps unused targets while 24 MiB of video memory is free). Against the default: `[hitch]` lines naming a target at each goal, and `[mem]` cdram free."),
    run("pooltrim0", "Render target trim frees everything at once", "replay", {"MSC_POOL_TRIM_ONE": "0"}, group="smooth", tier=2,
        why="MSC_POOL_TRIM_ONE=0 (default 1 frees at most one object per present). Against the default: the longest frame at a trim."),
    run("housekeep0", "Trim passes scan every picture", "replay", {"MSC_VGX_HOUSEKEEP_BACKOFF": "0"}, group="smooth", tier=2,
        why="MSC_VGX_HOUSEKEEP_BACKOFF=0 (default 1: a program or texture trim pass that deleted nothing waits 30 pictures / 4 frames before it scans again; timing only). Against the default: `[housekeep]` lines (passes, passes that deleted nothing, passes skipped) and the CPU time of the render thread."),
    run("poolthrash0", "Pass objects trimmed and made again freely", "replay", {"MSC_POOL_THRASH_GUARD": "0"}, group="smooth", tier=2,
        why="MSC_POOL_THRASH_GUARD=0 (default 1: a pass-object or kept-target size made again within a minute of being trimmed is kept from then on). Against the default: `[pool]` lines (sizes pinned), `[match]` lines (pass objects let go) and `[hitch]` lines naming a target in play."),
    run("warmorder0", "Warm-up list in file order", "tour", {"MSC_VGX_WARM_ORDER": "0"}, group="smooth", tier=2,
        why="MSC_VGX_WARM_ORDER=0 (default 1: programs drawn in a match are marked in the list and linked first at the next start). Against the default after two sessions: the `[match] start` line (programs marked, linked first), 'warm-up queue ended at the slot limit' lines and programs linked during a match."),
    run("warmspread0", "Shader warm-up: the old absolute guard, no spread", "tour", {"MSC_VGX_WARM_HEAP_MIB": "44", "MSC_WARM_MATCH": "0", "MSC_WARM_BLOCK": "0"}, group="smooth", tier=1,
        why="MSC_VGX_WARM_HEAP_MIB=44 MSC_WARM_MATCH=0 MSC_WARM_BLOCK=0 (the r7-r13 behaviour: the warm-up stops at picture 0 because the 96 MiB heap is over 44 MiB in use at start-up; default now: a floor on the free heap, the match's own programs (cap 300) in picture 1, copy and off-screen draws never left out). Against the default: the `[match] start` line (programs resident at kick-off: about 31 here, many more by default), `[warm]` lines, draws left out for a program that was not ready, main-scene and feeding a copy (`[match] end`, `[hitch] window`, `sk` in a `[hitch]` line), shader compiles and links in play, boot time to `present 2`, and the `[mem]` lines (heap free, FAILED counts) to see what the larger warm-up costs in memory."),
    run("warmblock0", "Draws that feed a copy may be left out for a program not ready", "replay", {"MSC_WARM_BLOCK": "0"}, group="smooth", tier=2,
        why="MSC_WARM_BLOCK=0 (default 1: a copy or off-screen draw compiles and links at once). Against the default: `[match] end` draws left out feeding a copy, and by eye the goalie's texture in a replay close-up."),
    run("slipcap0", "No cap on the catch-up steps", "replay", {"MSC_SLIP_STEPS": "0"}, group="smooth", tier=2,
        why="MSC_SLIP_STEPS=0 (default 2: a late frame advances the game by at most two 20 ms steps, single player only). Against the default: the hold mix in the `[present]` line (percent of pictures shown 3+ refreshes in live play) and the `[fps] slip` line (frames cut, ms not advanced). Estimate from a model: fewer holds of three refreshes and more; the game clock runs a little behind real time after a long hitch."),
    run("slipbudget0", "Slip without the budget (stateless rule)", "replay", {"MSC_SLIP_BUDGET_MS": "0", "MSC_PACE_LOG": "0"}, group="smooth", tier=2,
        why="MSC_SLIP_BUDGET_MS=0 (the Vita plan sets 30 since wave 9: a late frame is cut only from a 30 ms bucket that fills at 2 % of real time, so a sustained overload runs the game at 98 % of real time, not 83 %). Against the default: the `[slip] match #n` lines (game time lost to slip of the match's play time, the worst 2 s window, fixed steps a second of play: 50.00 is exact) and by eye whether the game is in slow motion while frames are late; the stateless rule loses up to 17-20 % of game time in a scene that is late every frame. MSC_PACE_LOG=0 rides on this run (the [pace] lines are a diagnostic with no effect on timing beyond a log line a window). Single player only. Host table: native/tools/fps/run_slip_budget_test.sh."),
    run("fxcomp0", "Effects drawn at full size (compositor off)", "mega", {"MSC_FX_COMPOSITE": "0"}, group="picture", tier=1,
        why="MSC_FX_COMPOSITE=0 (the build default is 1: additive and soft effects go to one reduced target and are composited once). Against the default on Mega Strikes, Storm Ship lightning, Sand Tomb: GPU busy ms, late frames, and by eye the ball's glow edge, lightning arcs, cut-scene thorns."),
    run("megasharp0", "Mega Strike ball sharpness rules off (the 1.3 effects target as before)", "mega", {"MSC_MEGA_SHARP": "0"}, group="picture", tier=2,
        why="MSC_MEGA_SHARP=0 (the build default is 1: in Mega Strike scenes the effects target has no 224-line cap and its depth copy takes the nearest depth of a texel, so the glow no longer overwrites the ball's edge in 3 pixel steps; the captain's launch and the result cut-scenes count as Mega scenes). Against the default on a Mega Save and its launch cut-scene: by eye the ball's edge and glow, GPU busy ms and late frames in those scenes, the `[megasharp]` line (buffers drawn with the rules, effect runs)."),
    run("megaballs0", "Mega Strike flying balls: depth-edge composite off (the MSC_MEGA_SHARP nearest-depth copy as before)", "mega", {"MSC_MEGA_BALLS": "0"}, group="picture", tier=2,
        why="MSC_MEGA_BALLS=0 (the build default is 1: in Mega Strike buffers the effects pass reads the picture's depth at each texel centre and the composite takes, at an edge, the light of the texel whose depth matches the pixel's, so the balls that fly in keep a full-size outline instead of a 4-pixel staircase ring). Against the default on a Mega Save: by eye the balls' outline and glow (shots), GPU busy ms and late frames in the Mega scene (the composite reads 5 depth and 2 colour texels a pixel against 1), the `[megaballs]` line (runs, composites, screens each)."),
    run("megasharp2", "Mega Strike ball sharpness also in the launch and result cut-scenes", "mega", {"MSC_MEGA_SHARP": "2"}, group="picture", tier=2,
        why="MSC_MEGA_SHARP=2 (the build default is 1: the rules in the Mega minigame only). 2 also makes the captain's launch and result cut-scenes Mega scenes for the renderer (held size, one sample, no scene policy, effects target without the 224-line cap). Against the default on The Dump and one lighter stadium: GPU busy ms, fps and late frames in the launch and result scenes (a heavy stadium may miss 30 there), `[megasharp] effects target` line (made at first use, video memory free), `[mem]` FAILED counts, by eye the ball in the launch scene."),
    run("celebminpx0", "Celebration effects runs as before (small runs get their own reduced pass)", "replay", {"MSC_FX_CELEB_MINPX": "0"}, group="picture", tier=2,
        why="MSC_FX_CELEB_MINPX=0 (default 900000 with the compositor on: in cut-scene and replay scenes a run must cover 900000 estimated pixels to get its own reduced pass; smaller runs are drawn in place). Against the default on The Dump and Lava Pit goal celebrations: GPU busy ms, late frames in the celebration, by eye the sparks and smoke edges."),
    run("heldaa-same", "Replays and cut-scenes keep 2x in every stadium", "replay", {"MSC_PICTURE_HOLD_MSAA_HELD": "same"}, group="picture", tier=2,
        why="MSC_PICTURE_HOLD_MSAA_HELD=same (default auto: Crystal Canyon, The Dump and Sand Tomb draw replays, goal celebrations and walk-outs with one sample and the effects compositor, where the model says 2x with effects in place misses the 30 fps budget). Against the default on The Dump and Sand Tomb goal celebrations: GPU busy ms, late frames in the celebration, the `[fxaa]` lines (samples and compositor runs per scene class); by eye the edges in the celebration."),
    run("heldaa-all1", "Replays and cut-scenes as r15 (auto: one sample only where the table says)", "replay", {"MSC_PICTURE_HOLD_MSAA_HELD": "auto"}, group="picture", tier=3,
        why="The r15 held-scene samples (auto) against the r16 default 0 (one sample in every stadium, owner 14 Nov 16:55): GPU ms and held-scene fps in a replay, compositor acted counts"),
    run("aapolicy0", "No per-stadium anti-aliasing table", "tour", {"MSC_AA_POLICY": "same"}, group="picture", tier=2,
        why="MSC_AA_POLICY=same (default auto: Dump, Lava Pit, Bowser Stadium and Sand Tomb play at one sample from kick-off, Crater Field and Crystal Canyon and Sand Tomb draw replays and cut-scenes at one sample; MSC_AA_TABLE=id:live:held,... corrects entries without a rebuild). Against the default at those stadiums: GPU busy ms and late frames in play (budget 15.0 ms), the [fxaa] policy and per-class lines."),
    run("warm1", "Other sample mode's program variants drawn at load (warm-up ON)", "tour", {"MSC_FXAA_WARM": "1"}, group="smooth", tier=2,
        why="MSC_FXAA_WARM=1 (default 0 until the shapes floor and warm10 are proven on a console; wave 10: the shader list's variants are drawn once into a small target of each other sample count after load; the r9 boot crash is fixed, the small target is kept). Needs a second session (the list is on the card). Against the default: the hitch at the first Mega Strike cut and the first replay or celebration at an auto stadium; the [fxaa] warm and variants lines; the [cuts] match line (variants)."),
    run("cutsshapes1", "Pass objects of the cuts made before kick-off (shapes ON)", "tour", {"MSC_CUTS_SHAPES": "1"}, group="smooth", tier=2,
        why="MSC_CUTS_SHAPES=1 (default 0 until proven on a console; wave 10: the pass objects an earlier session made after kick-off, the chains of the goal cut-scene, replay and celebration, are listed on the card (<list>.shapes) and made again before kick-off, one a present, 16 MiB at most and only while the video memory floor holds). Needs a second session. Against the default on the goal cuts: the [hitch] target counts and the late presents at the cuts, the [cuts] match line (first uses after kick-off; targets), [mem] FAILED counts and video memory free (it holds up to 16 MiB more)."),
    run("scenepin1", "Pass objects of a match kept for the match (pin ON)", "tour", {"MSC_POOL_SCENE_PIN": "1"}, group="smooth", tier=2,
        why="MSC_POOL_SCENE_PIN=1 (default 0 until proven on a console; wave 10: the pass objects a loaded match makes, and the ones MSC_CUTS_SHAPES makes ahead, stay for the match, up to 16 MiB, while 12 MiB of video memory are free; below 24 MiB free the older rule deleted an unused pass object after 60 presents, so every goal cut made its chain again). Against the default: the [cuts] lines (TRIMMED pass objects, pins held, targets first used after kick-off), the [hitch] target counts at the goal cuts, [mem] FAILED counts and video memory free."),
    run("runmerge1", "Effects runs joined across draws that clear them (run merge ON)", "mega", {"MSC_FX_RUNMERGE": "1"}, group="picture", tier=3,
        why="MSC_FX_RUNMERGE=1 (default 0; host: 12.3 to 10.6 runs a frame in The Dump's celebration, other stadiums about the same). Against the default on a goal celebration at The Dump and a Mega Strike: GPU busy ms, late frames, by eye the smoke and sparks edges (one accumulation instead of two)."),
    run("scenejoin0", "Crowd and shadow pictures separate again (join and half-size shadows off)", "tour", {"MSC_VGX_ATLASJOIN": "0", "MSC_VGX_SHADOWDOWN": "0"}, group="picture", tier=1,
        why="MSC_VGX_ATLASJOIN and MSC_VGX_SHADOWDOWN, on by default in 1.3 (open play 6 to 4 scenes in the host census). Against the default: GPU busy ms and late frames in open play; by eye the crowd edges and soft shadow edges."),
    run("fxcomp-w7on", "Effects compositor with the wave 7 additions switched on", "mega", {"MSC_FX_COMPOSITE_BOX": "1", "MSC_FX_COMPOSITE_DITHER": "1", "MSC_FX_COMPOSITE_SOFT": "1", "MSC_FX_COMPOSITE_EDGE": "1"}, group="picture", tier=2,
        why="the run rectangle, ordered pattern, third-size soft layers, farthest-depth copy and the goal crowd trim all ON (they default off). Against the default on Mega Strike, Storm Ship, Thunder Island, a goal celebration: GPU busy ms, late frames, the ball's edge, the floor glow, haze banding."),
    run("glowdead0", "Glow chain with its unread last level", "replay", {"MSC_FX_GLOW_DEAD": "0"}, group="picture", tier=2,
        why="gpu25: MSC_FX_GLOW_DEAD=1 is on in the Vita plan: the glow chain does not draw its last level when the composite reads an earlier one (exact on the host, 172 Dump pictures identical; scenes in a walk-out picture 15 -> 14). Off = the game's seven levels. Compare [beph] scenes a picture (about 1 more with it off), [lane] gpu and be ms in the walk-out and a goal replay; expect gpu +1 to +2 ms and be +1.5 ms with it OFF (estimate, 1.1 ms a scene plus a quarter-screen pass). The [glow] MSC_FX_GLOW_DEAD line must appear in the ON runs."),
    run("heldsize0", "Held scenes at the full 960x544 (no smaller held size)", "tour", {"MSC_HELD_SIZE_DOWN": "0", "MSC_HEAD_STAT": "1"}, group="picture", tier=2,
        why="held30: MSC_HELD_SIZE_DOWN=1 is on in the Vita plan (console 18 Nov: the 30 path drew walk-outs and cut-scenes at 784x544 1x and measured 35 ms where the Quality hold at 960x544 measured 47): walk-outs, goal cut-scenes and replays are drawn at 864x488 (the Quality floor) with one sample. This run is the A/B off side (960x544): run it with fb-tour and fb-crep on Vice and compare [head] class cut-scene p50, [lane] gpu, and the [hold] 'picture size' lines (with it on: one change to 864x488 at the walk-out, one back at kick-off; any 'NEW: made in this frame' at the cut is a remake: stop and say). The picture is 19 % smaller in pixels in those scenes only."),
    run("heldglow0", "Glow chain redrawn every picture in held scenes", "replay", {"MSC_HELD_GLOW_HALF": "0"}, group="picture", tier=2,
        why="held30: MSC_HELD_GLOW_HALF=1 is on in the Vita plan (owner 17 Nov 23:23): in walk-outs, cut-scenes and replays the glow chain's six blur passes are redrawn every other picture (the composite reads the chain of the previous picture). This run is the A/B off side: compare [beph] scenes a picture (about 3 more on average with it off: 6 on every second picture), [lane] gpu / be / fe in the walk-out, a goal cut-scene and a replay (expect gpu +3, be +4.5, fe +1 ms with it OFF, estimate: 1.1 ms a scene, be 1.5 ms a scene), and the [glow] MSC_HELD_GLOW_HALF line in the ON runs. By eye: the haze along the stadium's silhouettes in the walk-out lags one picture behind the camera on every second picture."),
    run("heldglow2", "Glow chain redrawn every third picture in held scenes", "replay", {"MSC_HELD_GLOW_HALF": "2"}, group="picture", tier=3,
        why="held30: the stronger setting of MSC_HELD_GLOW_HALF (two of three pictures reuse the chain): only if heldglow0 shows the walk-out or the replay still over 33 ms and the picture is clean to the eye."),
    run("fsheld0", "Replay frames sent on the send worker again (MSC_FS_HELD off)", "crep", {"MSC_FS_HELD": "0", "MSC_HEAD_STAT": "1", "MSC_HITCH_LINES": "1500"}, group="picture", tier=1,
        why="rep27: MSC_FS_HELD=1 is on in the Vita plan: a replay's frame is sent on the game thread (a replay has no simulation step, core 0 has room) instead of on the frame-send worker, which shares core 1 with the render thread's front end (console 18 Nov s29: replay game thread p50 37-40 ms of which most is the send join, core 1 at 86 %, replay interval p50 47 ms). This run is the A/B off side; run it alternating with the default (2 pairs) on fb-crep at Vice. Compare: replay interval p50/p95 ([frame-timing] task=8), [head] class replay game ms (expect the ON side lower: no join wait; work about 22-28 ms), [lane] cores (core 1 share down in replay windows), [present] held 2/3/4+ in replay windows (more 2s with it ON), the [fs] 'MSC_FS_HELD' counter line in the ON runs. Stop if the ON side's replay game ms p50 is over 33 (the game thread would then be the limit)."),
    run("fsheld2", "Replay and cut-scene frames sent on the game thread (MSC_FS_HELD=2)", "crep", {"MSC_FS_HELD": "2", "MSC_HEAD_STAT": "1", "MSC_HITCH_LINES": "1500"}, group="picture", tier=2,
        why="rep27: the wider setting (also TASK_NIS: goal cut-scenes, celebrations, the walk-out). NOT exact against the default: on the host the walk-out's plain send differs from the pipelined one in ~820 of 1000 pictures (the same difference MSC_FRAMESEND=off has; replays are identical). Only if fsheld0 shows the ON side helps replays: compare [head] class cut-scene p50 and the walk-out p50 against the default, and look at the walk-out by eye."),
    run("r30-rep", "Replay lanes split", "crep", {"MSC_VGX_BEPH": "1", "MSC_HEAD_STAT": "1", "MSC_HITCH_LINES": "1500", "MSC_GSPLIT": "2"}, group="picture", tier=2,
        why="held30 task 1: the replay's [lane] split (game / send / fe / be cpu and wall / gpu) next to the goal cut-scene's, with the back end's phase split ([beph] wall vs cpu per phase, scenes and draws a picture). The host census finds a replay picture no heavier than the cut-scene's (same scenes, fewer draws), the console (s29-int4) has the replay at p50 46.9 against 35.3: read which lane differs (expect GPU 30-36 and fe 22-27 in the replay window against 25-30 / 22 in the cut-scene) and whether be wall minus cpu (the wait behind the GPU) is larger in the replay."),
    run("glowcoarse", "Glow chain in 4 passes", "replay", {"MSC_FX_GLOW_COARSE": "1"}, group="picture", tier=2,
        why="gpu25: the game's own 4-pass glow route (MSC_FX_GLOW_COARSE=1; OFF by default). 2 of the 13-15 scenes of every walk-out / cut-scene picture that has the glow go away (host scene log, w8/gpu25.md); estimate gpu -2 ms and be -2 to -3 ms a picture. Compare [beph] scenes a picture (13-15 without, 11-13 with), [lane] gpu and be ms in the walk-out and a goal replay, and the [glow] counter line. Picture: host crops show a faint darker halo around characters in the walk-out; the owner judges it on the console."),
    run("asyncmiss", "New shaders compiled in the background (forced misses)", "tour", {"MSC_VGX_FS_PROJ": "1", "MSC_WARM_CAP": "40"}, group="draw", tier=2,
        why="exit17 (the 23 Nov console run stalled in this run: the whole run sat in the start-up warm-up, 'PREPARING THE SHADERS', 1190 of 5608 units, every fragment program compiled on the render thread at ~350 ms with '[vgx-sync] still waiting' up to 10 s, 225 s, no route): FS_PROJ rewrites the fragment text of every program that reads a projective coordinate, so with the default MSC_WARM_CAP 400 the picture-1 warm-up (compiles on the render thread BY DESIGN, with a progress bar) missed the pack for all of them and never reached the match. MSC_WARM_CAP=40 makes only 40 programs at picture 1 (about 15 s) and leaves the rest of the list to first use, which is where the background path (AsyncShaderSlot) applies: look for the '[warm]' line (40 asked, the rest left to first use) and then '[vgx] async-shaders-5s:' lines in the match. MSC_VGX_ASYNC_SHADERS (on, the Galaxy port's r101): MSC_VGX_FS_PROJ=1 (off; it only rewrites the fragment programs that read a projective coordinate, so it makes fragment programs the pack does not hold, met in play; MSC_VGX_DISCARD_BLEND is on in the plan since wave 16 and the pack has its programs, so it can no longer force misses). Check the '[genstat] ... MSC_VGX_FS_PROJ: N fragment shaders with texture2DProj' line: N above 0 or there were no forced misses. Delete the card's shader cache first for a second go (the console's cache keeps what it compiled). Read the '[vgx] async-shaders-5s:' lines (compiled in the background, draws skipped, longest wait, blocking path counts), any 'GUEST' or crash, the C heap line, and the [frame-timing] max / p99 of the first match windows against d1: a 40-190 ms compile on the render thread was the 1.2 behaviour. Look at the shots for objects that are missing for more than a moment (a skipped draw is left out at most 0.4 s). A crash or a picture fault in the first match here means MSC_VGX_ASYNC_SHADERS=0 in the release."),
    run("async0", "New shaders compiled on the render thread again", "tour", {"MSC_VGX_ASYNC_SHADERS": "0"}, group="draw", tier=2,
        why="MSC_VGX_ASYNC_SHADERS=0 (1.2 behaviour). With the pack nothing is compiled either way, so d1 and async0 should match within noise: the '[vgx] async-shaders-5s:' lines of d1 should show only shaders found in the cache. A difference beyond noise (frame-timing p50, the render lane) means the probe costs time and the default needs another look."),
    run("heaponly0", "Shader compile worker may take vitaGL's pools again", "tour", {"MSC_VGX_ASYNC_HEAPONLY": "0"}, group="draw", tier=2,
        why="MSC_VGX_ASYNC_HEAPONLY=0 (gxvgx14; on in the plan: the shader compile worker's vglMalloc family takes newlib's heap only, a block newlib cannot give fails the compile and the render thread compiles the shader itself). Never run on a Vita before: d1 and heaponly0 should match within noise. Read the '[vgx] async-shaders-heap:' line of d1 (blocks from the C heap, refused, foreign reallocs): any refused block, a 'GUEST' or crash, or a longer first-match [frame-timing] max / p99 than heaponly0 means the default is wrong (=0 in the release)."),
    run("discardblend0", "Alpha-tested blended draws with discard again", "tour", {"MSC_VGX_DISCARD_BLEND": "0"}, group="draw", tier=2,
        why="MSC_VGX_DISCARD_BLEND=0 (on in the plan since wave 16; pixel-exact by host proof, 61 pictures and 2M blend/z combinations, sgx16, and its fx-additive programs are in pack27): an alpha-tested draw that is blended so that a source of 0 changes nothing and writes no depth writes vec4(0) where its test fails instead of discarding, so the SGX keeps hidden-surface removal for it. This run is the A/B off side: the plain discard. Against d1: GPU busy ms ([rt]/[gpu]) in play with items and in Mega Strikes, expect off to be 0.1-0.3 ms slower (ESTIMATE); the off programs are in the pack as well, so no compile in either run. Any picture difference in d1 (black or dark fringes on effects) means DISCARD_BLEND goes back to off."),
    run("clockdefer0", "Clock requested at the very start again", "tour", {"MSC_VITA_CLOCK_DEFER": "0"}, group="cpu", tier=2,
        why="MSC_VITA_CLOCK_DEFER (on: the clocks are requested 1 s into the process, the Galaxy port's r99 GXV_CLOCK_DEFER). Read both logs' 'clocks:' lines: with a PSVshellPlus profile, d1 should show 'requested ARM none (the core already measures ~498 MHz)' and clockdefer0 may show 'measured 333/444 before' and a 500 refused then 444. The '[clock] boot:' and 30 s 'clocks: the ARM core measures' lines say what the core really ran at; fps and game-thread ms follow."),
    run("edgeaa0", "SMOOTH without edge smoothing", "tour", {"MSC_VITA_EDGE_AA": "0"}, group="picture",
        why="The present pass may cost more than the 3D scene (about 89 instructions a pixel against 29 by the compiler's count, an estimate). Already in the shader pack. Compare the GPU timers and the picture."),
    run("upfast", "Fast upscale", "tour", {"MSC_VITA_UPSCALE": "fast"}, group="picture",
        why="Three fetches at vertex-computed coordinates. Not in the shader pack, so its first start compiles it once; judge the later windows."),
    run("hudnative", "HUD and menus at the screen's size", "tour", {"MSC_VGX_HUDNATIVE": "1"}, group="picture", tier=2,
        why="The 2D layers go to the display over the upscaled scene. Look at score, clock and menu text, wipes and pause. GPU time should stay within noise: a HUD costs about 1.8x its own pixels, the EFB scene loses them."),
    run("upcombined", "SMOOTH's look from three fetches", "tour", {"MSC_VITA_UPSCALE": "combined"}, group="picture", tier=1,
        why="Against the reference (SMOOTH) and upfast (FAST). The display step should fall from about 8 ms to about 2 by the count: read the GPU time of the display scene and judge edges and text by eye. Not in the shader pack until the pack is regenerated: the first start compiles it once; judge the later windows."),
    run("upcas", "Contrast-adaptive upscale", "tour", {"MSC_VITA_UPSCALE": "cas"}, group="picture", tier=2,
        why="Five fetches, no dependent read (SMOOTH six, one dependent). Not in the shader pack: the first start compiles it once; judge the later windows."),
    run("fshalf1", "Half-precision shader arithmetic", "tour", {"MSC_VGX_FS_HALF": "1"}, group="picture", tier=2,
        why="Compiles its variants once at the first boot of this run (a long progress bar). Run it twice and judge the second. Read `[vgx] shader forms:` and any MSC_VGX_FS_HALF refusals."),
    run("fstrim1", "Fragment shaders without idle clamps", "tour", {"MSC_VGX_FS_TRIM": "1"}, group="picture", tier=2,
        why="Same pixels by proof and host test, fewer USSE instructions on the console's compiler. Every fragment text changes, so the first boot compiles the list (long progress bar) and first uses in play hitch. Run it twice and judge the second: GPU time against d1."),
    run("palmerge1", "Skinned meshes' palette runs in one draw", "tour", {"MSC_VGX_PALMERGE": "1", "MSC_VGX_PALMERGE_STATS": "1"}, group="draw", tier=2,
        why="Adjacent player meshes that differ only in their bone matrices are drawn as one call from a combined buffer (same pixels by construction and host test). Records with more than 30 matrix rows use a 60-row vertex shader that is not in the 1.3 pack: they compile on first use, so run it twice and judge the second. [palmerge] lines give the draws saved; compare [lane] backend ms, GPU ms and p50 with d1."),
    run("pmwave8", "Player draw merging: wave 8 set", "tour", {"MSC_VGX_PALMERGE": "1", "MSC_VGX_SKINMERGE": "1", "MSC_VGX_PALMERGE_MB": "8", "MSC_VGX_SKINMERGE_MB": "4", "MSC_VGX_PALCOMPACT": "1", "MSC_VGX_PALREGROUP": "1", "MSC_VGX_PALMERGE_STATS": "1"}, group="draw", tier=2,
        why="Wave 8 set, all OFF in the plan until the crowd-band pixels are explained: palette runs and GPU-skin runs from combined buffers (8 + 4 MiB of kept vertices), only the matrices a part names (PALCOMPACT), runs of one material regrouped inside a stretch of opaque player draws (PALREGROUP). Host: 30 to 45 % fewer player draw records in open play, but 1 to 2 crowd-band pixels still differ in 5 of 25 host frames. Against the tour: [lane] backend ms, GPU ms, p50/p99, C heap peak, crowd pictures."),
    run("sbatch1", "Static stadium batches (props of one material in one draw)", "tour", {"MSC_VGX_SBATCH": "1", "MSC_VGX_SBATCH_STATS": "1"}, group="draw", tier=1,
        why="Rigid stadium draws of one material that differ only in their matrices are drawn from a batch buffer (each member checked against its own matrices every frame; no shader change, pack 15 serves it). [sbatch] lines give the records saved, buffer MB and kept MB; compare [lane] backend ms, GPU ms, p50 and the C heap peak with d1. By eye: props, goals, nets and moving machinery in place, nothing missing."),
    run("sbatch2", "Static batches with proven reordering", "tour", {"MSC_VGX_SBATCH": "1", "MSC_VGX_SBATCH_ORDER": "1", "MSC_VGX_SBATCH_STATS": "1"}, group="draw", tier=1,
        why="As sbatch1, and opaque stadium records move back to their material's run when the move provably cannot change a pixel (disjoint rectangles or depth ranges). More records saved; FE ms against sbatch1 shows the cost of the proof."),
    run("vstrim1", "Vertex shaders without unit light terms", "tour", {"MSC_VGX_VS_TRIM": "1"}, group="draw", tier=2,
        why="Light attenuations that are exactly 1 leave the text, and colour 1 where nothing reads it. Keys change, so these shaders compile on first use in the match until the pack and list are remade: run it twice, judge the second, then the [lane] backend ms and GPU time against d1."),
    # (1.3: MSC_FLOOR_BAKE=7,8 and MSC_FLOOR_SHEEN=vertex are build defaults (vita_plan.cpp kSpeed), so the A/B turns them off on Pipeline Central; "none" names no stadium.)
    run("floorbake0", "Pitch reflection as the game draws it (floor bake off), Pipeline Central", "pipe", {"MSC_FLOOR_BAKE": "none"}, group="stadium", tier=2,
        why="The default merges the pitch reflection into the diffuse (three textures become one). Against pipe1 and pipe2: GPU time; by eye, the pitch with its full moving reflection. Slower here means the default pays."),
    run("floorsheen0", "Floor bake without the per-vertex sheen, Pipeline Central", "pipe", {"MSC_FLOOR_SHEEN": "none"}, group="stadium", tier=2,
        why="The default keeps the moving sheen per vertex on the baked floor. Against pipe1: GPU time should match; the game thread or front end pays the per-vertex work. By eye: the pitch goes flat while the camera moves."),
    run("hazewide", "Wider haze thinning", "tour", {"MSC_FX_HAZE_WIDE": "1"}, group="stadium", tier=2,
        why="Caustics, ash and wind thinned. Run on Lava Pit and Crystal Canyon as well."),
    run("hazecache3", "Haze drawn every 3rd frame into a small picture", "pipe", {"MSC_FX_HAZE_CACHE": "3", "MSC_AMORT_LOG": "1"}, group="stadium", tier=2,
        why="MSC_FX_HAZE_CACHE. Against pipe1, pipe2. First check `[amort] haze, 300 frames:` (drawn about a third, held two thirds) and `[fxstat]` runs near 0. GPU time should fall most in the stadiums with haze. By eye, with the camera following the ball: mist that swims or steps, mist in front of what it used to go behind. Also on Crystal Canyon and Wastelands with MSC_FX_HAZE_WIDE=1."),
    run("hazecache3heavy", "Haze picture with the heavy-stadium profile", "pipe", {"MSC_FX_HAZE_CACHE": "3", "MSC_FX_HAZE": "1"}, group="stadium", tier=2,
        why="The heavy-stadium profile is the build's default on Pipeline Central; this draws all the mist's sprites at a sixteenth of the pixels instead of half of them, none thrown away. Against pipe1 and defaults-all-pipe: `pipe-play` p50, and which haze looks better."),
    run("shadowplay3", "Visible players' shadow pictures every 3rd frame", "tour", {"MSC_SHADOW_PLAY": "3"}, group="stadium", tier=2,
        why="MSC_SHADOW_PLAY. A third fewer shadow scenes among visible players; the shadow's shape lags 100 ms at 30 fps, its place does not. `[rt] scenes` per frame, `play` p50. By eye: legs of a running shadow."),
    run("amort60ref", "Locked 60, the reference of amort60", "tour", {"MSC_FPS": "60"}, group="pacing", tier=2,
        why="MSC_FPS=60 alone, so that the only difference to amort60 is the switch."),
    run("amort60", "At 60, crowd and shadow pictures keep the rate of 30", "tour", {"MSC_FPS": "60", "MSC_AMORT_60": "1"}, group="pacing", tier=2,
        why="MSC_AMORT_60. Against amort60ref. `[rt] scenes` per frame should fall by about 2 in open play (shadows and crowd sprites at half the frames), `play` p50 and the share of frames at 16.7 ms rise. By eye: nothing that was not already true at 30."),
    run("shadowplane", "Planar shadows on the game's plane", "tour", {"MSC_SHADOW_PLANE": "1"}, group="stadium", tier=2,
        why="Render-only picture fix. Watch the ground shadows of hammers, power-ups and stadium objects: flicker or gaps at 0, steady at 1. No time cost expected."),
    run("gxmdirect1", "Scene draws in one vitaGL call each", "tour", {"MSC_VGX_GXM_DIRECT": "1"}, group="draw",
        why="MSC_VGX_GXM_DIRECT, off until this A/B. Galaxy measured +1 fps and -1 ms in the back end. The [gxm-direct] line must show a refused count of 0; any corrupt geometry rejects it."),
    run("copyinplace0", "Shadow-volume copy made again (A/B of the 1.3 default)", "tour", {"MSC_VGX_COPYINPLACE": "0"}, tier=2, group="draw",
        why="MSC_VGX_COPYINPLACE, on by default in 1.3: the full-screen alpha copy before the shadow-volume quad is left out. `[rt] scenes` per frame is 1 higher here in open play, replays and cut-scenes, GPU p50 should be higher here by an estimated 0.5-1 ms at 960x544. By eye the two runs must match: the dark shadow-volume patches under goal posts, walls and the stands' edges, in play and in the goal replay. Any difference in those patches rejects the default."),
    run("between", "Frame Rate UP TO 60", "tour", {"MSC_FPS_BETWEEN": "pace"}, group="pacing",
        why="Settings row FRAME RATE UP TO 60. Keep it if play shows a quarter more pictures, [present] judder under 0.1 and [lat] p50 up by under a refresh. Modelled only so far."),
    run("hitch1", "Long frames attributed, replay route", "replay", {"MSC_HITCH_MS": "45", "MSC_LOAD_TRACE": "1"}, group="hitch",
        why="Diagnostics only: frame times stay valid. [hitch] names the cause of every frame gap of 45 ms or more (shader, target, texture, wait, display, backend, upstream), [load] the match load's timeline. Also read `[vita] thread sweep` for what the 600-frame sweeps cost."),
    run("loadfps60", "Match load at a picture per refresh", "replay", {"MSC_LOAD_FPS": "60", "MSC_LOAD_TRACE": "1"}, tier=2, group="hitch",
        why="Compare the [load] total and frames with hitch1. Keep it if the load is shorter; the loader's frames are about 85 a match, so at best it saves about 1.4 s. Solo only by construction."),
    run("sweepbg0", "Thread sweeps back on the game and render threads", "tour", {"MSC_VITA_SWEEP_BG": "0", "MSC_HITCH_MS": "45"}, tier=2, group="hitch",
        why="MSC_VITA_SWEEP_BG=0 (the Vita plan sets 1 since wave 9: the two 600-frame thread sweeps, at least 500 system calls each, run on the watch thread every 30 s). Against the default: the `[hitch]` lines and the `[pace] display` fold by picture % 600 (a bin of holds of 3+ at frames 600, 1200 ... with the sweep on the game thread), `[vita] thread sweep ... took` us."),
    run("syncraise12", "UP TO 60 with a later rise of the picture latency", "tour", {"MSC_FPS_BETWEEN": "pace", "MSC_PRESENT_SYNC_RAISE": "12"}, tier=2, group="pacing",
        why="On the model a 48 fps scene keeps L 5 instead of 6: 17 ms less delay for judder 0.11 against 0.02. Compare [lat] p50 and [present] judder with the run `between`."),
    run("snaponce0", "Packet copy at the attach and at the fork, as 1.2", "tour", {"MSC_FS_SNAPONCE": "0"}, group="cpu",
        why="MSC_FS_SNAPONCE is on in 1.3: exact, the same bytes for the send. This is its A/B: game thread ms and p50 against d1."),
    run("pktsort0", "Packets filed into the sort tree on the game thread and the full power-up search, as 1.2", "tour", {"MSC_PKT_SORT": "0", "MSC_POWERUP_FILTER": "0"}, tier=2, group="cpu",
        why="MSC_PKT_SORT and MSC_POWERUP_FILTER are on in 1.3: exact (same packets, same order; per-frame vgx hash equal on the armhf harness). The A/B: game thread ms in [lane] and p50 against d1; the send column may rise a little (the sort moved there)."),
    run("irqwait", "Interrupt lock waits of the game thread (audio callback)", "tour", {"MSC_IRQ_WAITSTAT": "1"}, tier=2, group="cpu",
        why="Diagnostic, nothing changes. The [irq] lines every 5 s: ms the game thread waited for the AX frame callback's hold of the interrupt lock, and the callback's hold time. Under ~0.5 ms a second there is nothing to win by moving game audio; Galaxy's audio-on-core-1 does not apply otherwise (no guest sound thread here)."),
    run("gamefast0", "Game thread shortcuts off, as 1.2", "tour", {"MSC_ODE_BROADPHASE": "0", "MSC_POSE_FAST": "0"}, group="cpu",
        why="MSC_ODE_BROADPHASE and MSC_POSE_FAST are on in 1.3: exact, same simulation (per-step digests compared). The A/B says what they save: game thread ms in [lane] and p50 against d1."),
    run("armcpu0", "Worker flag through emulated thread-local storage and tan() every time, as 1.2", "tour", {"MSC_FS_TLSFAST": "0", "MSC_PERSP_TAN": "0"}, tier=2, group="cpu",
        why="MSC_FS_TLSFAST and MSC_PERSP_TAN are on in 1.3: exact (the same answers, only found without the Vita's emulated TLS call chain of about 90 instructions, 3000 times a picture; tan memoised by its exact argument). This is their A/B: game thread ms in [lane] and p50 against d1. MSC_FS_TLSVERIFY=1 (not in this run) compares every answer with the old one and prints MISMATCH on the first difference."),
    run("cpuexact0", "The three wave-13 exact CPU changes off, as 1.3 before them", "tour", {"MSC_TLS_FAST": "0", "MSC_TICKS_FAST": "0", "MSC_FPBITS": "0"}, tier=2, group="cpu",
        why="MSC_TLS_FAST, MSC_TICKS_FAST and MSC_FPBITS are on in 1.3 (wave 13, cpuexact13): exact (host proofs in docs/TECHNICAL.md: unit tests against copies of the library code, every ns below 2^50, all 2^32 float patterns; det trace equal). The A/B: game thread ms in [lane] and p50 against d1 (estimate 0.1-0.3 ms together). With the default the log has a [tls] line (on, offset, emutls wrapped); here it says off."),
    run("tlsverify1", "Fast thread-local lookup with every call checked against the library's", "tour", {"MSC_TLS_FAST_VERIFY": "1"}, tier=2, group="cpu",
        why="MSC_TLS_FAST_VERIFY=1 runs the library's lookup next to the fast one on every call (slower: not a timing run). The [tls] line must say on, and no '[tls] MISMATCH' line may appear in the whole log. This is the console proof of the TLS wraps."),
    run("prefetch15", "Software prefetch in the game thread's three miss-heavy loops and the recorder", "tour", {"MSC_PREFETCH": "15"}, tier=2, group="cpu",
        why="MSC_PREFETCH is 0 in 1.3 (bits: 1 node-matrix build, 2 character blend, 4 recorder write pointer, 8 skin pose). A prefetch changes no value, only timing (host: det trace and frame hashes equal). The A/B: game thread ms in [lane] and p50 against d1; keep the bits that lower it (kit runs can set MSC_PREFETCH=1, 2, 4, 8 one at a time)."),
    run("fpscr1", "Flush-to-zero on the game thread", "tour", {"MSC_VITA_FPSCR": "1"}, tier=2, group="cpu",
        why="MSC_VITA_FPSCR is 0 (nothing changed, only logged) in 1.3. The [fpscr] line of ANY run says what the main thread starts with; this run sets flush-to-zero (a denormal reads as 0): game thread ms in [lane] and p50 against d1. Keep only if the game thread drops and the route ends the same."),
    run("posecull0", "Players off the screen posed again", "tour", {"MSC_POSE_CULL": "0"}, group="cpu",
        why="MSC_POSE_CULL=1 is on in 1.3: exact. The A/B says what it saves on the game thread."),
    run("crowdposeonce0", "Crowd sprites posed one by one, as 1.2", "tour", {"MSC_CROWD_POSEONCE": "0"}, group="cpu",
        why="MSC_CROWD_POSEONCE is on in 1.3: exact."),
    run("crowdrefresh1", "Crowd refresh back to every picture under the match camera", "tour", {"MSC_CROWD_REFRESH": "1"}, group="cpu",
        why="MSC_CROWD_REFRESH=2 is on in 1.3 and is the one visible item of the group: the sprites move half as often. If this run is no slower than d1, 2 should go back."),
    run("npclazy0", "Decorative NPCs pose as 1.2 (with the crowd hidden)", "tour", {"MSC_NPC_LAZY": "0", "MSC_PERFORMANCE_MODE": "1"}, group="cpu",
        why="MSC_NPC_LAZY only acts with Stadium Crowd HIDE, so this run hides the crowd; its reference is perfhide (the same with the switch on), not d1."),
    run("perfhide", "Stadium Crowd HIDE (reference for npclazy0)", "tour", {"MSC_PERFORMANCE_MODE": "1"}, group="cpu",
        why="What npclazy0 is compared with; it also says what hiding the crowd saves in 1.3."),
    run("texmemo0", "Every texture bind walks the inventories, as 1.2", "tour", {"MSC_GL_TEXMEMO": "0"}, group="cpu", tier=2,
        why="MSC_GL_TEXMEMO=1 is on in 1.3: exact, the same texture indices. This is its A/B: the send's share of the [lane] line and p50 against d1."),
    run("texmemo2", "Texture memo checked against the walk", "tour", {"MSC_GL_TEXMEMO": "2"}, group="cpu", tier=2,
        why="A correctness run, not a speed run: log.txt must hold no line `[gl] texture memo MISMATCH`."),
    run("promosolo8", "Lone small meshes get a static buffer after 8 replays", "tour", {"MSC_VGX_PROMOSOLO": "8"}, group="draw",
        why="MSC_VGX_PROMOSOLO, off until this A/B. Compare the FE and back end ms; a hitch at the first seconds of a stadium would be the uploads."),
    run("dltiny64", "Display lists up to 64 bytes skip the cache", "tour", {"MSC_VGX_DL_TINY": "64"}, group="draw",
        why="The Galaxy value (Strikers default 15). Not a console-proven gain here."),
    run("fxglowhalf", "Glow chain one level shorter", "tour", {"MSC_FX_GLOW": "half"}, group="picture",
        why="Settings row GLOW LIGHTER. Menus and, in stadiums with high-range objects, replays. First confirm from [memo] whether the pass memo already skips the chain in menus (SCENE-AUDIT section 2)."),
    run("rep1", "1.3 defaults, replays (first reference)", "replay", role="ref", group="reference",
        why="Reference pair of the replay route: replays and celebrations are pooled over up to five goals, marked rep:."),
    run("shadow2", "Replay and cut-scene shadows every 2nd frame", "replay", {"MSC_SHADOW_EVERY": "2"}, group="replay",
        why="Settings row REPLAY SHADOWS. Open play's schedule. Risk: one stale shadow frame after a camera cut. Proposed on if rep:replay beats rep1/rep2 and it looks right."),
    run("farlod12", "Replay far players with the open-play mesh", "replay", {"MSC_REPLAY_FARLOD": "12"}, group="replay",
        why="Settings row REPLAY FAR PLAYERS. Risk: mesh pop at the distance, no hysteresis. Stays off until seen."),
    run("rep2", "1.3 defaults, replays (second reference)", "replay", role="ref", group="reference",
        why="Second reference of the replay route."),
    run("fxruns2", "At most 2 effect runs a frame at reduced size, Mega Save", "mega", {"MSC_FX_LOWRES_RUNS": "2"}, group="picture",
        why="MSC_FX_LOWRES_RUNS. Each run costs two more scenes and a composite; Galaxy limited runs to 2 after 10 extra scenes and 9 ms of renderer CPU bought no pictures. [fxstat] gives runs per buffer."),
    run("clock444", "Clock held at 444 MHz (what 500 gives the game thread: the A/B against d1)", "tour", {"MSC_VITA_CPU_MHZ": "444", "MSC_VITA_CLOCK_DEFER": "1"}, group="clock", tier=2,
        why="clock17/clockfix17. d1 runs the player's policy (500 asked 1 s after the start; the owner's PSVshellPlus profile and CapUnlocker are on his Vita, as for Galaxy). Here exactly 444 is asked ALSO 1 s into the process (MSC_VITA_CLOCK_DEFER=1 with MSC_VITA_CPU_MHZ=444), after the profile has been applied. The PSVshellPlus profile of the test title must NOT lock 500 during these two runs (switch it off or unlock its CPU): a lock answers 0 and changes nothing, and the run would be 500 against 500. Read the `clocks: policy ... (MSC_VITA_CPU_MHZ)` line (requested ARM 444) and, because that line's `measured after` is taken at the request, the later `clocks: the ARM core measures N MHz now` line (30 s): it must say about 444 here, ~498 in d1. Then the game thread's ms in [frame-timing]. Clock speed changes no result, only the time a step takes. Not battery or heat: 500 costs more of both."),
    run("clock444d", "Clock held at 444 MHz, The Dump", "worstgpu", {"MSC_VITA_CPU_MHZ": "444", "MSC_VITA_CLOCK_DEFER": "1"}, group="clock", tier=2,
        why="clock17/clockfix17: as clock444 (same 1 s defer, same profile condition) on the heaviest stadium, against worstgpu1 (which runs at 500)."),
    run("clockmax", "Clock policy of the player build (max: a 500 request, 444 if refused)", "tour", {"MSC_VITA_CLOCK_POLICY": "max"}, group="timing",
        why="Since clock17 d1 already runs policy max, so this is identical to d1 by construction (a repeat of d1, not a separate test). Read the `clocks:` line: requested ARM 500 (or none with about 498 measured under a profile)."),
    run("pipecensus", "Pipeline Central census (frame times void)", "pipe", CENSUS, role="census", group="stadium",
        why="SCENE-AUDIT section 1: [fgcensus], [stadium-census], [bypass], [texlog], ~layer rows and [fxstat] convict or clear candidates C1 to C6. First on this route: it also warms the shader cache."),
    run("ctrlcensus", "Control census in Bowser Stadium (frame times void)", "ctrl", CENSUS, role="census", group="stadium",
        why="The same census in a stadium that has not been reported slow: the difference is the evidence."),
    run("worstgpu1", "1.3 defaults, The Dump (worst stadium for GPU in the host census)", "worstgpu", tier=1, group="stadium",
        why="The host census (w8 stadiums, budget model, estimates) ranks The Dump worst for the GPU: celebration 31.9 ms and open play 15.7 ms at 960x544 against 25.7 and 9.9 for the match60 stadium (Vice). Five goals with celebration and replay in one run. Compare with tour (Vice): frame time and GPU time in open play and in the celebration."),
    run("pipe1", "1.3 defaults, Pipeline Central (first reference)", "pipe", role="ref", group="reference",
        why="The real frame rate of the reported 6-7 fps stadium in 1.3, on a warm cache. Also read `[vgx-vita] GPU queue depth ... the GPU takes X ms`: near the frame time is GPU, far below is CPU or uploads."),
    run("pipe2", "1.3 defaults, Pipeline Central (second reference)", "pipe", role="ref", group="reference",
        why="Noise of this route."),
    run("dmid", "1.3 defaults (mid-session reference)", "tour", role="ref", group="reference",
        why="The session now lasts over three hours: a third reference between d1 and d2 shows drift (heat, battery) in the middle, and widens the noise if there is any."),
    # ---- the switches of 1.2, still undecided -----------------------------------------------------------------------------------------
    run("msaa0", "Anti-aliasing off (the larger, supersampled sizes instead)", "tour", {"MSC_VGX_MSAA": "0"}, group="picture",
        why="Settings row ANTI-ALIASING OFF. The default has 4x then 2x as ladder rungs at the game's size, climbed to after a second of GPU room, in place of the 1.06-1.2 sizes."),
    run("fxn", "Effects quality FASTER (cut-scene effects at low resolution)", "tour",
        {"MSC_FX_LOWRES_SCENES": "nis", "MSC_FX_LOWRES_MINPX": "480000"}, group="picture",
        why="Settings row EFFECTS QUALITY FASTER, as the Settings screen writes it. Changes the picture a little in cut-scenes."),
    run("fg0", "Prioritize Players off", "tour", {"MSC_DYNRES_FG": "0"}, group="picture",
        why="Settings row PRIORITIZE PLAYERS OFF: the crowd refresh step is gone, the whole scene shrinks first (the texture-LOD step needs MSC_FG_LODBIAS, tier 2)."),
    run("aniso0", "Distant textures plain (anisotropic filtering off)", "tour", {"MSC_VITA_ANISO": "0"}, group="picture",
        why="Settings row DISTANT TEXTURES OFF. Untested on a console either way: does the filter cost frame time, does it look right (looks run)."),
    run("dumpmat-aniso0", "The Dump: anisotropic filtering off", "worstgpu", {"MSC_VITA_ANISO": "0"}, group="picture", tier=2,
        why="gpu25: The Dump's main stadium material is ONE fragment shader of four texture layers (each with its own uv: ((T1 x T1.alpha x K x T2) + vertex colour x T0) x T3, 22 instructions) that shades about 0.7 of a screen at 960x544 in open play (host FSCENSUS), about 2.7 of the ~7.7 fetch-screens it shades; all four are trilinear + anisotropic 2. Against the default on the same route: [lane] gpu ms p50/p95 in open play, then the far half of the pitch by eye. Gain here says what the filter is worth on this material (expect 0.5 to 2 ms if the fill is texture bound; estimate, no basis on the console)."),
    run("dumpmat-animin512", "The Dump: anisotropic filtering only on 512-texel textures", "worstgpu", {"MSC_VITA_ANISO_MIN": "512"}, group="picture", tier=2,
        why="gpu25: MSC_VITA_ANISO_MIN=512 (default 128): the stadium's 128 and 256 texel layers keep plain trilinear, the pitch's big textures stay anisotropic. Same reads as dumpmat-aniso0; a smaller picture change."),
    run("dumpmat-swzmips", "The Dump: swizzled layout also for textures with mip levels", "worstgpu", {"MSC_VGX_TEXSWIZZLE": "1", "MSC_VGX_TEXSWIZZLE_MIPS": "1", "MSC_VGX_TEXCLASS": "1", "MSC_HEAD_STAT": "1"}, group="hardware", tier=3,
        why="gpu25: at The Dump 446 of 585 live textures (20.2 of 29.4 MiB) have mip levels and 23.7 MiB are uncompressed LINEAR; the default swizzle (swz-d) skips every mip-mapped texture, so it cannot reach the stadium material. This turns the mip chains in too (the case that once drew a wrong HUD icon; swz20 notes). Run swz-d first; this one only if it is clean. Reads: [lane] gpu, every HUD icon, [texclass] outcomes; any wrong icon = stop."),
    run("dumpmat-texcompact", "The Dump: compact texture formats (I8/IA8 one byte, CMPR-alpha as DXT5)", "worstgpu", {"MSC_VGX_LA88": "2", "MSC_VGX_CMPRA": "2"}, group="hardware", tier=2,
        why="gpu25: at The Dump 93 CMPR textures with transparent texels (7.1 MB) are held as 32-bit RGBA8 and go DXT5 under MSC_VGX_CMPRA=2 (colour within 12 of 255 or the texture stays decoded; alpha exact), the 14 I8 textures (4.6 MB) one byte a texel. A quarter of the texture bytes for those draws, compressed textures are swizzled already. Reads: [lane] gpu ms in open play, the [vgx] texture lines (live MiB), by eye the stadium textures with alpha."),
    run("timing0", "Pacing and pipeline items off together: queue depth, vblank priority, core 0, pace by vblank, bounded slip", "tour",
        {"MSC_VITA_QUEUE_FIXED30": "0", "MSC_VITA_PRIO_VBLANK": "0", "MSC_VITA_CORE0_ALONE": "0", "MSC_PACE_VBLANK": "0", "MSC_SLIP": "0"}, group="timing",
        why="Screening run for the five pacing items at once (all on in the default). The Galaxy port found that its own pipeline switches, all on by default, cost it frames: "
            "the Observatory went from 38.1 to 40.6 fps with them off. Whether the same holds here is open and only a console can say: compare the frame time p50/p95 and the "
            "late-frame count with d1/d2. Only if it differs from the noise do the single runs pay off (core0off is in the session as the first of them; the others are tier 2)."),
    run("core0off", "Threads may run on core 0 again", "tour", {"MSC_VITA_CORE0_ALONE": "0"}, group="timing",
        why="Single run of one timing0 item, the first to pay off: MSC_VITA_CORE0_ALONE moves the library threads (audio, file, network) off core 0 so the game thread has it alone, "
            "the largest scheduling change of the five. If timing0 beats the noise and this run does too, core 0 is the cause; if only timing0 does, run the tier-2 singles."),
    run("hold0", "AUTO frame rate: the ladder hold off (a GPU-limited miss at 60 drops to 30 at once)", "tour", {"MSC_FPS_LADDERHOLD": "0"}, group="pacing",
        why="MSC_FPS_LADDERHOLD=1 is on in the default: a GPU-limited miss at 60 waits (8 s at most) while the dynamic resolution still has picture steps to give, and a CPU-limited one drops to 30 at once. "
            "This is the A/B on the match route, the busiest of the three: compare the share of the match at 60 (the [fps] auto lines and the frame-timing windows), the number of 60 <-> 30 switches, "
            "the late-frame count and the picture level (the [vgx] dynamic resolution lines) with d1/d2."),
    run("exact0", "Draw cuts off together: shadow quad uncut, rigid merge limit 128, depth of field as the game", "tour",
        {"MSC_VGX_SHQUAD": "0", "MSC_VGX_SOFTXFORM": "128", "MSC_DOF": "1"}, group="draw",
        why="Screening run for the three draw-cost items at once (all on in the default; the first and third draw the same picture, SOFTXFORM 256 moves a few pixels by rounding)."),
    run("dscn2", "Display render target with 2 scenes per frame, Mega Save", "mega", {"MSC_VITA_DISPLAY_SCENES": "2"}, group="timing",
        why="STR-068/069, Galaxy VITA_DISPLAY_SCENES: the display's own render target may take a second scene in a frame without waiting (default 1). "
            "Compare the [lat] p50/p95 and the frame interval p95 with megad1 / megad2; it adds no shader variants."),
    # ---- is the hardware used at full speed (hwspeed, 2026-11-05) --------------------------------------------------------------------------
    run("gpu166", "GPU core at 166 MHz instead of 222", "tour", {"MSC_VITA_GPU_MHZ": "166", "MSC_DYNRES": "0"}, group="hardware", tier=2,
        why="Fixed picture size, or the ladder hides the answer: use gpufix as its reference. A PSVshell profile with the GPU locked ignores the request: run without one, and the `clocks:` line must report GPU 166. If the GPU time per frame ([lane] gpu, [rt] GPU busy) grows by the full 222/166 = 1.34, the core (shaders, fill) "
            "is the limit and memory is not; a smaller growth is the share spent waiting on memory. Also proves 222 is real: a getter is not a measurement."),
    run("xbar111", "GPU crossbar at 111 MHz instead of 166", "tour", {"MSC_VITA_XBAR_MHZ": "111", "MSC_DYNRES": "0"}, group="hardware", tier=2,
        why="Against gpufix. Growth of the GPU time here is the share of the frame that waits on the GPU's path to video memory. Near zero: placement in video memory cannot be the limit."),
    run("bus166", "Bus at 166 MHz instead of 222", "tour", {"MSC_VITA_BUS_MHZ": "166", "MSC_DYNRES": "0"}, group="hardware", tier=2,
        why="Against gpufix. Growth of GPU time is the share waiting on main memory (shader code, vertices, uniforms); the CPU lanes will move too, so read [lane] gpu, not the frame rate."),
    run("gpufix", "1.3 defaults at a fixed picture size (reference of the clock runs)", "tour", {"MSC_DYNRES": "0"}, group="hardware", tier=2,
        why="Reference for gpu166, xbar111, bus166, pb32 and pb8."),
    run("gpusplit", "GPU time split into tiling and pixels", "tour", {"MSC_VITA_RT_SPLIT": "1", "MSC_VITA_RT_TIMERS": "3", "MSC_VGX_VSCENSUS": "300"}, group="hardware", tier=2,
        why="`[gpu split]`: how much of the GPU's time the picture size can act on (Galaxy: tiling about 12 ms, pixels 24-26). With `[vscensus]` it says whether vertex work is worth cutting. Frame times are about the same as d1: one more notification a scene."),
    run("ussecdram", "Shader code in video memory", "tour", {"MSC_VITA_USSE_CDRAM": "1", "MSC_DYNRES": "0"}, group="hardware", tier=2,
        why="MSC_VITA_USSE_CDRAM against gpufix: [lane] gpu. Same pictures. 8 MiB less video memory for textures: read the pool lines of log.txt; any `no GPU memory` line fails the run."),
    run("ringcdram", "Uniform ring in video memory", "tour", {"MSC_VITA_VGL_RING_MEM": "cdram", "MSC_DYNRES": "0"}, group="hardware", tier=2,
        why="An old switch of 1.2 never compared on a 1.3 base. Against gpufix: [lane] be (the CPU writes to it, uncached main memory is about 0.25 GB/s by the 5 Oct measurement) and [lane] gpu (the GPU reads it)."),
    run("streamcdram", "Frame vertex and index data in a video memory arena", "tour", {"MSC_VITA_VGL_STREAM_KIB": "2048", "MSC_DYNRES": "0"}, group="hardware", tier=2,
        why="As ringcdram, for the frame's GL_STREAM_DRAW buffers. 6 MiB of video memory (three display buffers). The `stream arena` overrun count in the [vgx] vitaGL line must stay 0 or the arena is too small for this route."),
    run("streamcached", "Frame vertex and index data in a cached arena", "tour", {"MSC_VITA_VGL_STREAM_KIB": "2048", "MSC_VITA_VGL_STREAM_MEM": "cached", "MSC_DYNRES": "0"}, group="hardware", tier=2,
        why="As streamcdram, with the CPU writing at cache speed and a write-back per frame through kubridge (the log line says which). A stale picture means the write-back does not reach memory: fails the run."),
    run("texswizzle", "Uncompressed textures in the swizzled layout", "pipe", {"MSC_VGX_TEXSWIZZLE": "1", "MSC_DYNRES": "0"}, group="hardware", tier=2,
        why="MSC_VGX_TEXSWIZZLE against pipefix, on the route with the heaviest multi-texture floors. Same pictures: any difference fails it, far mip levels first. [lane] gpu is the result; [texlayout] says how much was swizzled; loading time is the price."),
    run("cmpra", "CMPR textures with transparent texels as DXT5", "pipe", {"MSC_VGX_CMPRA": "1", "MSC_DYNRES": "0"}, group="hardware", tier=2,
        why="Against pipefix. A quarter of the bytes of what is RGBA8 today, in the textures that matter for transparent fill (alpha cut-outs). Colours may move a little: check the crowd and cut-out edges. Read the video memory lines too."),
    run("tex16", "16-bit textures where nothing is lost", "pipe", {"MSC_VGX_TEX16": "1", "MSC_DYNRES": "0"}, group="hardware", tier=2,
        why="Against pipefix. Textures without mip levels and from 128 x 128 up, whose texels all fit 565, 5551 or 4444: half the bytes, the same texels but for the GPU's c / 31 expansion (under half a step). 2 (mip levels too) is not exact and is not the run."),
    run("lai8", "I4 and I8 textures as one byte, IA4 and IA8 as two", "pipe", {"MSC_VGX_LA88": "2", "MSC_DYNRES": "0"}, group="hardware", tier=2,
        why="Against pipefix. 80 % of the RGBA8 bytes of a match are GX I8 textures; this sends them as vitaGL's GL_INTENSITY (U8_RRRR). Same four numbers per texel: any difference in fonts, digits, shadows or fades fails the run (a swapped or missing alpha would show at once). Read [glmem]/[mem] video memory and the texture lines of the statistics."),
    run("cmpra2", "CMPR cut-outs as DXT5 within a stated colour error", "pipe", {"MSC_VGX_CMPRA": "2", "MSC_DYNRES": "0"}, group="hardware", tier=2,
        why="Against pipefix. Like cmpra, but a texture whose middle colour would move by more than 12 of 255 stays RGBA8; the statistics line says how many did."),
    run("pipefix", "1.3 defaults at a fixed picture size, Pipeline Central", "pipe", {"MSC_DYNRES": "0"}, group="hardware", tier=2,
        why="Reference for texswizzle."),
    run("pb32", "Parameter buffer 32 MiB, Mega Save", "mega", {"MSC_VITA_PB_MIB": "32", "MSC_DYNRES": "0"}, group="hardware", tier=2,
        why="A partial render shows only as a GPU spike. Against megafix: [lane] gpu p95 and the longest frames. Steadier here means the 16 MiB default overflows in heavy moments."),
    run("pb8", "Parameter buffer 8 MiB, Mega Save", "mega", {"MSC_VITA_PB_MIB": "8", "MSC_DYNRES": "0"}, group="hardware", tier=2,
        why="Against megafix. No change means 8 MiB of video memory are free for textures, and pays for ussecdram."),
    run("megafix", "1.3 defaults at a fixed picture size, Mega Save", "mega", {"MSC_DYNRES": "0"}, group="hardware", tier=2,
        why="Reference for pb32 and pb8."),
    # ---- 1.2, 1.1 and 1.0: their own eboot and shader pack (session.sh swap) --------------------------------------------------------------
    run("r12", "Version 1.2 (the unreleased freeze)", "tour", version="1.2", role="version", group="version", why="The headline comparison: 1.2 has never had a matched Vita measurement."),
    run("r12m", "Version 1.2, Mega Save", "mega", version="1.2", role="version", group="version", why="Regression reference."),
    run("r11", "Version 1.1 as shipped", "tour", version="1.1", role="version", group="version", why="Regression reference."),
    run("r11m", "Version 1.1 as shipped, Mega Save", "mega", version="1.1", role="version", group="version", why="Regression reference."),
    run("r10", "Version 1.0 as shipped", "tour", version="1.0", role="version", group="version", why="Regression reference."),
    run("r10m", "Version 1.0 as shipped, Mega Save", "mega", version="1.0", role="version", group="version", why="Regression reference."),
    # ---- back on the candidate: the second reference run, after everything else (drift, heat) -----------------------------------------------------------
    run("d2", "1.3 defaults (second reference)", "tour", role="ref", group="reference",
        why="Second reference run, last in the session: its difference from d1 is the noise (and drift) of the whole session."),
    run("megad2", "1.3 defaults, Mega Save (second reference)", "mega", role="ref", group="reference",
        why="Noise for the Mega Save scene."),
    # ---- looks: short runs with screenshots, steady 30 fps so a frame number is the same match moment --------------------------------
    run("lookbase", "Look: 1.3 defaults at 30 fps", "look", dict(SHOTS), group="look", role="look",
        why="Picture reference for the look runs."),
    run("looklod2", "Look: sharper textures", "look", dict(SHOTS, MSC_VGX_LODBIAS="-2"), tier=2, group="look", role="look",
        why="A quarter of a mip level sharper on every mipmapped texture. Compare the far half of the pitch still and in motion: crisper is the aim, shimmer is the risk. Then one tour run with the same line for the GPU timers: finer levels read more memory."),
    run("lookshape", "Look: picture in the screen's shape", "look", dict(SHOTS, MSC_VITA_SHAPE="1"), tier=2, group="look", role="look",
        why="712x400 for 640x448: the same pixels, scaled alike on both axes. Compare the far pitch and players (should gain) with the HUD and menu text (loses height, may soften). Then one tour run for the GPU timers: pixels are not more, tiles differ."),
    run("lookmsaa0", "Look: anti-aliasing off", "look", dict(SHOTS, MSC_VGX_MSAA="0"), tier=2, group="look", role="look",
        why="Edges against lookbase (the default climbs to 4x/2x when the GPU has room; off, the supersampled sizes take that place)."),
    run("lookfxn", "Look: effects quality FASTER", "look", dict(SHOTS, MSC_FX_LOWRES_SCENES="nis", MSC_FX_LOWRES_MINPX="480000"), tier=2, group="look", role="look",
        why="Walk-out glow and haze against lookbase."),
    run("lookaniso0", "Look: distant textures plain", "look", dict(SHOTS, MSC_VITA_ANISO="0"), group="look", role="look", why="Far half of the pitch against lookbase."),
    run("lookctrim0", "Look: crowd sprite quads uncut", "look", dict(SHOTS, MSC_VGX_CROWDTRIM="0"), tier=2, group="look", role="look",
        why="MSC_VGX_CROWDTRIM (each crowd sprite quad cut to the box the figure was drawn in) is on in the default. The 9 open-play shots against lookbase should differ by at most 2 levels of 255 on a few hundred pixels; the frame timing of the open-play part against lookbase says whether the CPU cost of the cut (about 0.5 ms per crowd frame on the host) is paid back by the GPU (about half of the crowd's fragments)."),
    # ---- tier 2: ready, not in the 90-minute session --------------------------------------------------------------------------------------------
    run("lookdof0", "Look: depth of field removed", "look", dict(SHOTS, MSC_DOF="0"), tier=2, group="look", role="look",
        why="Walk-out background against lookbase. The default (lazy) draws the game's own picture, so it needs no look run; =0 removes the blur."),
    run("lookfg0", "Look: Prioritize Players off", "look", dict(SHOTS, MSC_DYNRES_FG="0"), tier=2, group="look", role="look",
        why="Dropped from the session: the crowd step only acts under GPU pressure, so a look run at 30 fps with room to spare draws the same picture."),
    run("q30off", "GPU queue depth back to the auto rule", "tour", {"MSC_VITA_QUEUE_FIXED30": "0"}, tier=2, group="timing",
        why="Single run of one timing0 item (the Galaxy fixed-depth fix, which acts only while the rate is held at 30)."),
    run("vprio0", "vi-retrace priority back to the timer role's", "tour", {"MSC_VITA_PRIO_VBLANK": "0"}, tier=2, group="timing",
        why="Single run of one timing0 item."),
    run("pacev0", "Frame pacing from the frame clock only", "tour", {"MSC_PACE_VBLANK": "0"}, tier=2, group="timing",
        why="Single run of one timing0 item."),
    run("slip0", "Bounded slip off", "tour", {"MSC_SLIP": "0"}, tier=2, group="timing",
        why="Single run of one timing0 item. MSC_SLIP=20 is on in single player: a late frame advances the game by at most 20 % less than it took. Off, a late frame asks the next one for more steps."),
    run("shquad0", "Shadow quad uncut", "tour", {"MSC_VGX_SHQUAD": "0"}, tier=2, group="draw",
        why="Single run of one exact0 item (exact: the same picture; the cut saves fill rate where the shadow volumes are small)."),
    run("shqlive0", "Open-play shadow quad uncut", "worstgpu", {"MSC_VGX_SHQUAD_LIVE": "0", "MSC_VGX_LEDGERPAL": "0"}, tier=2, group="draw",
        why="gpu24: the full-screen shadow-resolve quad of open play (flat shadows) is cut to the area around the shadows by default (exact on the host: pictures equal). Off = the old full-screen quad. Compare [lane] gpu ms p50/p95 in play at The Dump (about 1 screen of blended fill, estimate 1.2-1.5 ms at 960x544)."),
    run("aniso0-d", "Anisotropic filtering off (the price of MSC_VITA_ANISO=2), The Dump", "worstgpu", {"MSC_VITA_ANISO": "0"}, tier=2, group="draw",
        why="tile24 item 2, zero code: the Vita plan has MSC_VITA_ANISO=2 (minimum 128 texels). Against the same route with the default: [lane] gpu ms p50/p95 in play at The Dump and in the held scenes. The price of the filter is the gap; if it is under 0.3 ms the scoped variant (anisoscope1) cannot gain more. Visible: far pitch and stands blur without it."),
    run("anisoscope1", "Anisotropic filtering only on opaque depth-writing draws, The Dump", "worstgpu", {"MSC_VITA_ANISO_SCOPE": "1"}, tier=2, group="draw",
        why="tile24 item 2 (vitaGL 0057 vglTexAnisoOff, default off, visible in principle): blended draws (effects, glow, particles, crowd) sample the same textures trilinear; the pitch, stands and ramps keep the filter. Against the default: gpu ms (expect a part of aniso0's gain) and a look at effects with a large texture (rain, haze, glow). Textures used by both kinds of draw change their descriptor each time the kind changes (counted in texParams)."),
    run("tiled1", "Tiled colour surfaces for pass targets and EFB copies, The Dump", "worstgpu", {"MSC_VGX_TILED_RT": "1"}, tier=2, group="draw",
        why="tile24 item 1 (vitaGL 0057 vglTexMakeTiledTarget, default off, console-only: the host does not model tiling). Pass targets and EFB-copy textures (RGBA8, 32+ texels each way) become SCE_GXM_COLOR_SURFACE_TILED with tiled textures; the EFB and the display stay linear. PASS if the picture is unchanged (any garbage, shear or black pass is a layout rule broken: stop and report which scene) and the [vgx] line says N tiled render-target textures; the gain is [lane] gpu ms in held scenes (walk-out, celebration, replay: about 15 scenes a picture) and in play. A crash at the first pass is also a result."),
    run("tiled2", "Tiled colour surfaces, whole-tile sizes only, The Dump", "worstgpu", {"MSC_VGX_TILED_RT": "2"}, tier=2, group="draw",
        why="tile24 item 1, the careful variant of tiled1: only pass targets and copies whose width and height are multiples of 32 (no padded storage, no partial tiles: 960x544, 768x768, 640x448). Run this one first; if tiled2 is clean and tiled1 is not, the partial-tile rule is the problem."),
    run("la883", "Mipmapped I4/I8/IA4/IA8 textures as one byte / two, The Dump", "worstgpu", {"MSC_VGX_LA88": "3"}, tier=2, group="hardware",
        why="tile24 item 3: LA88=2 plus the mipmapped intensity textures sent as level 0 in U8_RRRR / A8L8 and the chain generated by vitaGL (sceGxmTransferDownscale per channel, the same engine as for RGBA8; not provable on the host). The first [vgx] line says the startup self-test was exact. Compare with la882 the live texture MiB ([glmem]) and the pictures of fonts, shadows and fades; the line 'texture census (tile24)' says how many mipmapped I/IA textures this stadium has."),
    run("la882", "I4/I8 one byte a texel, IA as two (the base of la883), The Dump", "worstgpu", {"MSC_VGX_LA88": "2"}, tier=2, group="hardware",
        why="tile24 item 3 base for la883 (the same as lai8 on the worstgpu route)."),
    run("zero1", "Blended draws that provably change nothing are not drawn, The Dump", "worstgpu", {"MSC_VGX_ZERODRAW": "1", "MSC_VGX_ZERO_COUNT": "1"}, tier=2, group="draw",
        why="tile24 item 4 (exact by the blend arithmetic: final alpha 0 with SRC_ALPHA, or alpha and colour 0 with ONE/ONE; draws that write depth are never cut). The [zerodraw] line says how many such draws a picture holds; gain = their pixels, read [lane] gpu ms. Pictures must be unchanged."),
    run("dumpcut1", "Background cut table, The Dump", "worstgpu", {"MSC_STADIUM_CUT": "table"}, tier=2, group="draw",
        why="MSC_STADIUM_CUT=table against the same route without it (NOT exact: the background loses the heat shimmer, the large haze sprites are thinned, far small scenery is not drawn, the crowd refreshes every third frame; players, ball, shadows and HUD untouched; judged on [lane] gpu ms and late pictures first, then by eye on the pictures, three runs a side). The [stadiumcut] lines count what each bit did (DID NOTHING means it did not act). Default OFF until the owner has seen the pictures."),
    run("dumprain1", "The Dump rain, one streak in two", "worstgpu", {"MSC_STADIUM_CUT": "64"}, tier=2, group="draw",
        why="MSC_STADIUM_CUT=64 (bit 64 DUMPFX; the kit cannot carry the id:mask form, a colon is refused by the isolation reader) against the same route without it (NOT exact: The Dump's rain streaks, template fx_dump_rain, every second one is not built and the others are drawn at twice the alpha; host census: the 32x32 additive rain texture 0.72-0.80 -> 0.38-0.42 Mpx, the Dump's play fill 1.88-1.90 -> 1.52-1.53 Mpx; the sheet shows visibly sparser rain, which is why the table row stays 0). Judged on [lane] gpu ms and the late pictures, three runs a side, then the owner on a picture. The [stadiumcut] line counts 'rain streaks not built'. Default OFF."),
    run("dumpcut1l", "Background cut table, Lava Pit", "nlava", {"MSC_STADIUM_CUT": "table"}, tier=2, group="draw",
        why="MSC_STADIUM_CUT=table against the same route without it (NOT exact: the background loses the heat shimmer, the large haze sprites are thinned, far small scenery is not drawn, the crowd refreshes every third frame; players, ball, shadows and HUD untouched; judged on [lane] gpu ms and late pictures first, then by eye on the pictures, three runs a side). The [stadiumcut] lines count what each bit did (DID NOTHING means it did not act). Default OFF until the owner has seen the pictures."),
    run("gal0", "Galaxy r41 ports off", "tour", {"MSC_VGX_CULL": "0", "MSC_VGX_VGL_FRAGMEMO": "0", "MSC_VGX_VGL_UNIREUSE": "0", "MSC_DYNRES_SEVERE": "0"}, tier=2, group="draw",
        why="Static cull and the two vitaGL draw caches (exact: the same picture; judged on back end ms), and the deeper floor under a severe GPU overload (not exact, acts only at 1.8 times the budget)."),
    run("min070", "Smallest picture at 60: 0.70 per side", "tour", {"MSC_DYNRES_MIN60": "0.70"}, tier=2, group="picture",
        why="GPU shortlist item 1, in PERFORMANCE since 1.3: only this line changed. Record the sizes in the [vgx] dynamic resolution lines: a floor that is never reached is no win."),
    run("fx4", "Effects at a quarter of each side, Mega Save", "mega", {"MSC_FX_LOWRES": "4"}, tier=2, group="picture",
        why="GPU shortlist item 2, in PERFORMANCE since 1.3: only the divisor changed. Look for chunky glow and halos at depth edges."),
    run("perf12", "PERFORMANCE (60) as 1.2 wrote it", "tour", PERF12, tier=2, group="preset", why="Base for perf13."),
    run("perf13", "PERFORMANCE (60) as 1.3 writes it", "tour", PERF13, tier=2, group="preset", why="The whole mode against perf12. Decides whether its 1.3 values stay; min070, fx4, between, shadow2, farlod12 are the single items."),
    run("pipeperf", "PERFORMANCE (60) as 1.3 writes it, Pipeline Central", "pipe", PERF13, tier=2, group="preset", why="Does the mode make the worst stadium playable? Against pipe1 and pipe2."),
    run("latehold60", "PERFORMANCE (60) without the late-frame hold (A/B of the 1.3 default)", "tour", dict(PERF13, MSC_DYNRES_LATEHOLD="0"), tier=2, group="preset",
        why="MSC_DYNRES_LATEHOLD (strikersVita's drop-and-hold, on the GPU's own time). Against perf13: late frames and the longest gaps in [frame-timing] at 60, the count of 'a late frame (MSC_DYNRES_LATEHOLD)' level changes, and the scale the ladder sat at (softer picture is the price)."),
    # (1.3: MSC_STADIUM_HEAVY=8 with MSC_STADIUM_HEAVY_CROWD=0 is a build default, so the A/B turns the profile off; "none" names no stadium.)
    run("pipeheavy0", "Heavy-stadium profile off, Pipeline Central", "pipe", {"MSC_STADIUM_HEAVY": "none"}, tier=1, group="stadium",
        why="Against pipe1 and pipe2: the log must not say [heavy] stadium 8 ... renderer reductions applied. Pipeline Central is the players' top complaint (6-7 fps on 1.0/1.1): slower here confirms the default; the size of the gap is what the profile buys."),
    # Release 1.3: the mist switches proposed for after the console test (MSC_FX_TIGHT=8, MSC_FX_TIGHT_DROP=1, MSC_FX_GOV=1) all written off, on the same stadium at the default
    # picture mode. With today's defaults it is a third reference beside pipe1/pipe2; once the proposal lands it is that proposal's "off" run, and defaults-all-pipe the "on" run.
    run("pipemist0", "Mist switches off (tight sprites, faint-sprite drop, fill governor), Pipeline Central, default picture mode", "pipe",
        {"MSC_FX_TIGHT": "0", "MSC_FX_TIGHT_DROP": "0", "MSC_FX_GOV": "0", "MSC_FX_STAT": "1"}, tier=1, group="stadium",
        why="Against defaults-all-pipe (the proposed set on) and pipe1: pipe-play p50, GPU ms, [vgx] dynamic resolution steps and mean scale. If the proposed set is still under 30 here, hazecache3heavy is the next step."),
    run("crowdreplay2", "Crowd sprites and cluster every 2nd frame in replays", "replay", {"MSC_CROWD_REPLAY": "2"}, tier=2, group="replay",
        why="rep:replay and rep:nis against rep1, rep2. By eye: the crowd's stutter close to the camera."),
    run("clusterlazy", "No crowd cluster where nothing reads it", "replay", {"MSC_CROWD_CLUSTER_LAZY": "1"}, tier=2, group="replay",
        why="The same picture by reading. rep:replay against rep1, rep2: a win on this route's stadium means the capture was paid for nothing."),
    run("menulod1", "Menu captains with the low mesh", "tour", {"MSC_MENU_LOD": "1"}, tier=2, group="menu",
        why="menu-* p50 on the team and captain screens. By eye: flatter faces; every captain's textures present (the low mesh's materials have never been seen in the menus)."),
    run("menulod2", "Menu captains low, sprites every 2nd frame", "tour", {"MSC_MENU_LOD": "2"}, tier=2, group="menu",
        why="Against menulod1. By eye: half-rate captains, a blank sprite for one frame after a load."),
    run("menulod3", "Menus without the backdrop world (a test)", "tour", {"MSC_MENU_LOD": "3"}, tier=2, group="menu",
        why="menu-main against d1: how much of the main menu is the backdrop world. Not a mode to ship: the background is empty."),
    run("dof1", "Depth of field as the game (capture every frame)", "tour", {"MSC_DOF": "1"}, tier=2, group="draw",
        why="Single run of one exact0 item (lazy draws the same picture and skips a dead capture)."),
    run("dof0", "Depth of field removed", "tour", {"MSC_DOF": "0"}, tier=2, group="picture",
        why="Settings row DEPTH OF FIELD OFF. Cut-scene GPU saving; lookdof0 shows the picture."),
    run("msaa2", "Anti-aliasing 2x only (older ladder)", "tour", {"MSC_PICTURE_HOLD": "0", "MSC_VGX_MSAA": "2"}, tier=2, group="picture",
        why="The 1.2 row's 2X ONLY choice (removed from the 1.3 row) with FULL PICTURE OFF: the older ladder without its 4x rung (that ladder has 4x then 2x). With the full picture on (the default) the default is 2 samples: see aa0, aa2, aa4, aa864."),
    # 1.3 (w8 aa2x): the owner's four setups for the held picture's anti-aliasing (12 Nov), written out in full so they do not depend on the build's defaults. MSC_PICTURE_HOLD_MSAA is the held picture's samples
    # (renderer/src/vgx_hold.h, the plan's default 2); they are the first thing given up when frames run late, before any size step. Nothing here has run on a console: every cost is an estimate.
    run("aa0", "Full 960x544, no anti-aliasing", "tour", {"MSC_PICTURE_HOLD": "full", "MSC_PICTURE_HOLD_MSAA": "0"}, tier=2, group="picture",
        why="Owner's setup 1 (12 Nov): the full picture with one sample. It is the Settings row ANTI-ALIASING OFF with FULL PICTURE on. The reference for aa2, aa4 and aa864: [frame-timing] p50, p99, late frames and GPU ms in open play, and the [hold] lines (the video memory line must show held + ladder only)."),
    run("aa2", "Full 960x544, 2x anti-aliasing", "tour", {"MSC_PICTURE_HOLD": "full", "MSC_PICTURE_HOLD_MSAA": "2"}, tier=2, group="picture",
        why="Owner's setup 2: the build's default picture (2 samples), spelled out. Against aa0: what 2x costs in open play (estimate: about +13 percent of the pixel part of the GPU time), whether the policy gives the samples up in the heavy stretches ([hold] step lines naming 'samples to 1'), the video memory line ('multisampled held targets' about 6.1 MiB more than aa0, an estimate), a hitch at each swap (the 'picture size ... 2x' lines and [hitch] records around them), and by eye the edges. Mega Strikes and Pipeline Central draw with one sample by design."),
    run("aa4", "Full 960x544, 4x anti-aliasing", "tour", {"MSC_PICTURE_HOLD": "full", "MSC_PICTURE_HOLD_MSAA": "4"}, tier=2, group="picture",
        why="Owner's setup 3: 4 samples, given up 4 -> 2 -> 1 when frames run late. Against aa2: the cost of 4x (estimate: +40 percent of the pixel part) and how long the policy keeps it; the video memory line (4x + 2x + 1x targets, about 16.3 MiB more than aa0, an estimate: if the console has less free than 12 MiB + a target the '[hold] no 4x render target' line says which count was left out)."),
    run("aa864", "The floor size (864x488), 4x anti-aliasing", "tour", {"MSC_PICTURE_HOLD": "864x488", "MSC_PICTURE_HOLD_MSAA": "4"}, tier=2, group="picture",
        why="Owner's setup 4: a smaller held picture with 4 samples (800x454, the screen's aspect; the ladder steps below it are 0.90, 0.81, 0.73 of that). Against aa4 and aa2: whether 4x at 0.69 of the pixels of 960x544 holds 60 where 2x at full size does not, and which looks better by eye. The picture is scaled up to the screen by the present."),
    run("fglod", "Prioritize Players with the texture-LOD step", "tour", {"MSC_FG_LODBIAS": "4"}, tier=2, group="picture",
        why="TECHNICAL.md: a console test of the LOD step starts at 4 (Prioritize Players itself is on in the default)."),
    run("d30", "1.3 defaults, locked 30 fps", "tour", {"MSC_FPS": "30"}, tier=2, group="matched-30",
        why="Base for matched-30 comparisons (the validation spec asks for defaults first, then matched 30)."),
    run("q30offat30", "GPU queue depth back to the auto rule, locked 30 fps", "tour", {"MSC_FPS": "30", "MSC_VITA_QUEUE_FIXED30": "0"}, tier=2, group="matched-30",
        why="The fixed-depth fix switched off where it would act all the time; against d30."),
    run("latehold30", "Late frame steps down and holds 2 s, locked 30 fps", "tour", {"MSC_FPS": "30", "MSC_DYNRES_LATEHOLD": "2500", "MSC_DYNRES_LATE30": "1"}, tier=2, group="matched-30",
        why="Against d30. The model says nothing changes at 30 (two frames queued hide a single late frame); a gain here says the model is wrong and LATE30 should be on."),
    run("r11at30", "Version 1.1, locked 30 fps", "tour", {"MSC_FPS": "30"}, version="1.1", tier=2, role="version", group="matched-30", why="Against d30."),
    run("r10at30", "Version 1.0, locked 30 fps", "tour", {"MSC_FPS": "30"}, version="1.0", tier=2, role="version", group="matched-30", why="Against d30."),
    run("dbuf2", "Two display buffers instead of three", "mega", {"MSC_VITA_TRIPLE": "0"}, tier=2, group="timing",
        why="STR-068/069: vglUseTripleBuffering(false). One frame fewer waits in the display queue; watch that the frame rate holds."),
    run("replay30", "Replays and cut-scenes held at 30", "tour", {"MSC_FPS_REPLAY": "30"}, tier=2, group="pacing", why="CONSOLE-TESTS 1.2 row."),
    run("hyst1", "1.1 AUTO thresholds", "tour", {"MSC_FPS_HYST": "1"}, tier=2, group="pacing", why="CONSOLE-TESTS 1.2 row: flips per minute."),
    run("latdepthp1", "Free run keeps 3 frames in flight (the old depth)", "tour", {"MSC_LAT_DEPTH_PACED": "1"}, tier=1, group="pacing",
        why="MSC_LAT_DEPTH_PACED=1 (the Vita plan sets 2 since wave 9: an aligned free run keeps 4 frames in flight, the model gives 59.2 pictures a second against 52.3 at a GPU of 1.0 refresh, 56.4 against 49.0 at 1.05, for 4.3 ms more button delay at 1.0). Against the default in an UP TO 60 scene of 45-58 fps: `[fps] between:` pictures a second, the `[pace]` display line (held 1/2/3+, flip late) and `[lat]` p50. Keep 2 if the pictures rise by about 3 and the p50 grows by under 5 ms."),
    run("padlate0", "No late pad and touch read", "mega", {"MSC_PAD_LATE": "0"}, tier=1, group="pacing",
        why="MSC_PAD_LATE=0 (the Vita plan sets 1 since wave 10: under a held 30 the pad and the touch screen are read up to 12 ms later, in the slack the picture waited for its refresh). Against the default in a Mega Save and in a held 30 scene: the `[fps] late latch` lines (steps delayed, mean and max delay, late pictures), the `[pace] display` line's frame start to shown and estimated press to picture, the `[lat]` press-to-picture p50/p95 (measured), and the hold mix (it must not move). By hand: aim a Mega Save with the touch screen and time a shot or tackle: does it feel quicker, does anything feel late?"),
    run("phase1", "Late start with fault attribution (MSC_PHASE_SYNC=1), Vice tour", "tour", {"MSC_PHASE_SYNC": "1"}, tier=2, group="pacing",
        why="MSC_PHASE_SYNC=1 (pace10, default off; replaces MSC_PAD_LATE's controller, which stays on in the plan). The Galaxy port's phase sync with its fault attribution: a late picture with no delay in force is a spike and does not switch the late start off for 120 pictures (r11: 5 of 2400 steps delayed at Vice, 0 of 1200 at Lava Pit). Against the default, three runs a side: the `[fps] phase sync:` line (share of steps delayed, mean delay, late pictures charged to the delay) against the `[fps] late latch:` line, `[lat]` pad->shown p50 and p95 in live play (must go down or stay), the `[pace]` hold mix and `[hitch]` count (must not go up), `[speed]`. Host model: -1 to +1 ms, so only the log lines can say if it acts."),
    run("phase1dump", "Late start with fault attribution (MSC_PHASE_SYNC=1), The Dump", "worstgpu", {"MSC_PHASE_SYNC": "1"}, tier=2, group="pacing",
        why="MSC_PHASE_SYNC=1 at the stage where the late latch did act (r11 worstgpu1: 316 of 1200 steps delayed, mean 7.6 ms): the same lines as phase1; here a delay of 6-8 ms should show in `[lat]` p50 and must not add late pictures (9.3 % in r11, 5-12 % run to run: three runs a side)."),
    run("hbud90", "Held 30 picture budget aims at 90 % (Galaxy's headroom)", "tour", {"MSC_PICTURE_HOLD_BUDGET_MARGIN": "0.90"}, tier=2, group="pacing",
        why="MSC_PICTURE_HOLD_BUDGET_MARGIN=0.90 (the Vita plan has 0.80; the Galaxy port's GXV_HEADROOM_PCT aims at 90 % of the budget). Strikers already keeps MORE headroom than Galaxy did, so this is the opposite test: does the background quality come back (hold step, `[hold]` lines) without more late pictures ([pace] 3+ share, [fps] play fps)."),
    run("latdrain0", "Frames-in-flight gate stands aside after a long frame (no drain)", "tour", {"MSC_LAT_DRAIN": "0"}, tier=2, group="pacing",
        why="MSC_LAT_DRAIN=0 (the Vita plan sets 1 since wave 10: a held 30 that stood aside after a long frame still drains a backlog above depth + 1). Against the default after a goal celebration and a load at a held 30: the `[lat]` press-to-picture p50 in the 5 s after the long frame and the `[fps] frames in flight` line's drain count; the hold mix must not move."),
    run("clockslip", "Game clock: the wave 9 slip bucket instead of the debt clock, The Dump", "worstgpu", {"MSC_CLOCK": "slip", "MSC_SLIP_REFILL_PCT": "2"}, tier=1, group="pacing",
        why="MSC_CLOCK=slip and MSC_SLIP_REFILL_PCT=2: the wave 9 bucket exactly (defaults since pacing12: debt, 0.5). r9 The Dump read 92.7 % and about 79.5 % of real time in two windows and 95.4 % over the match (47.69 steps a second) while the slip ledger claimed 98.1 %. The debt clock advances a late picture by what the game thread can run (2-5 steps), owes the rest (at most 100 ms), forgives a stall over 250 ms and signals what it cannot run ([clock] OVERLOAD). Read the [speed] lines (every clean 5 s window must read 99 % or more; a hitch may dip one window to 98 %) and the [clock] lines (debt, shed, forgiven, overload entries), with the picture mix ([present] hold mix, [lane] fps) and pad->shown unchanged. Against this run the match percentage in [speed] match #1 end is the number."),
    run("bg30half", "Background at 30, players at 60 (MSC_BG30=2), Vice", "tour", {"MSC_BG30": "2", "MSC_FPS": "60"}, tier=1, group="bg30",
        why="MSC_BG30=2 (default off until this run says yes; the owner's 14:04 decision makes it the live-play behaviour). Live-play pictures alternate FULL (the background - stadium, crowd, sky, far scenery - is kept in a 960x544 texture) and LIGHT (no background drawn: the kept one is re-projected to the new camera through the pitch plane; players, ball, items, shadows, nets, effects and HUD fresh). Read: [bg30] (renderer: full/light per 600, fallbacks must be 0 or explained, corner motion), [bg30g] (game: kinds, game thread ms a picture by kind, crowd/stadium skips), [lane] fps / game / GPU against d1 (expect a lower GPU and game-thread mean per picture), [present] hold mix, [speed], [hitch], [lat]. By eye: players and ball smooth, no edge smear, no flicker of goal posts over players. A crash or a black background in a light picture is a failure."),
    run("bg30proof", "Background kept and re-used in the same picture (MSC_BG30=1, the exactness proof), Vice", "tour", {"MSC_BG30": "1"}, tier=2, group="bg30",
        why="MSC_BG30=1: every live picture copies its background prefix into the kept texture and draws it straight back (identical picture by construction; host-proven identical). On the console it measures what the capture costs: [lane] GPU against d1 (expected +1-2 ms: one scene split and a 960x544 copy) and checks the scene split keeps the EFB depth ([bg30] fallbacks 0, players not drawn over by the stadium)."),
    run("bg30dump", "Background at 30, players at 60 (MSC_BG30=2), The Dump", "worstgpu", {"MSC_BG30": "2", "MSC_FPS": "60"}, tier=1, group="bg30",
        why="MSC_BG30=2 at the heaviest stadium. Expected (estimate): the pair does not fit at 960x544 2x (full 28-30 + light 8-11 ms GPU); read [lane] GPU by picture kind (light pictures should be 8-11 ms) to size the background ladder."),
    run("bg30pair", "Background at 30 + balanced pair + pair controller (MSC_BG30=2, BALANCE=3, MSC_PAIR_RATE=1 on an even 30), Vice", "tour",
        {"MSC_BG30": "2", "MSC_BG30_BALANCE": "3", "MSC_PAIR_RATE": "1", "MSC_FPS": "30", "MSC_GSPLIT": "1", "MSC_RENDER_RNG": "1", "MSC_FRAMELOCK_STEP": "1", "MSC_VIS_STEP": "1"}, tier=1, group="bg30",
        why="The automatic behaviour of the owner's 14:04 decision as one switch set (default off until the owner has judged it on the console): even 30 until the pair controller (MSC_PAIR_RATE=1, np_pair_rate.h) is shown the pair fits (game thread and GPU, 1.5 s), then a FULL picture and a LIGHT picture per 33 ms, every picture one refresh; BALANCE=3 moves the crowd pose update and the stadium animation update into the light picture's slot; a camera cut or a pan over 12 px a picture (MSC_BG30_PANPX) makes the picture full. Read: [pair] lines (cadence on/off, why), [bg30g] (kinds, fast pans, balance counters, game ms by kind), [gsplit-bg30] (work phases by kind), [bg30] (renderer), [lane]/[fps] against d1 and bg30half, [pace] late pictures, [lat] p50/p95, [speed] 98 % rule, [hitch]. By eye: player motion smoothness first, then edge smear at the screen edge on a fast pan, crowd and stadium animation, goal posts over players. A crash, a black background or a [pair] flap every few seconds is a failure."),
    run("bg30pairdump", "Background at 30 + balanced pair + pair controller, The Dump", "worstgpu",
        {"MSC_BG30": "2", "MSC_BG30_BALANCE": "3", "MSC_PAIR_RATE": "1", "MSC_FPS": "30", "MSC_GSPLIT": "1", "MSC_RENDER_RNG": "1", "MSC_FRAMELOCK_STEP": "1", "MSC_VIS_STEP": "1"}, tier=1, group="bg30",
        why="bg30pair at the heaviest stadium. Expected (estimate): the pair does not fit at 960x544 2x (full 28-30 + light 8-11 ms GPU), so the controller must stay at the even 30 without flapping ([pair] lines: no more than one entry try per 2-32 s cool-down); this is the run that sizes the per-stadium background cut (MSC_BG30_CUTS)."),
    run("bp0", "Back-pressure before the pad off (wave 10 frames-in-flight limit)", "tour", {"MSC_LAT_BP": "0"}, tier=1, group="pacing",
        why="MSC_LAT_BP=0 (default since pacing11: 1). Held 30 at r9: pad -> shown p50 110 ms, of which about 33 ms is a finished picture waiting a whole 30 Hz interval for its flip behind a picture queued ahead of it, and 160-200 ms in heavy moments because the depth 2 limit stood aside or switched itself off. With it on a picture starts only when at most 1 earlier picture is unshown (depth found from the cadence, 1-4). Read the [lat] pad->shown lines and presented->handed (expect about 70-75 ms steady 30), the [fps] back-pressure lines (depth, waits, raises taken back), and that [lane] fps and the [present] hold mix are the same as the run without this one. Any lost frame rate or a new 2-refresh mix in heavy moments is a failure of the depth control, not of this run."),
    run("betweenmeasured", "Free-run gate judges the cost measured at 30 (wave 9 rule)", "tour", {"MSC_FPS_BETWEEN_COST": "measured"}, tier=1, group="pacing",
        why="MSC_FPS_BETWEEN_COST=measured (default since pacing11: free). The gate compares the busiest stage's cost at the FREE-RUN rate (per-picture part + one step of 5 ms) with its 20 ms limit; measured at a held 30 a picture carries 1.67 steps and Vice in PERFORMANCE mode read 21.7 ms against 18.4 free: the gate never opened (r9: locked 30 instead of 50-53 pictures a second). Against the default at Vice: `[fps] between:` lines (the cost it judged and how), pictures a second, the `[pace] display` hold mix (one-refresh share must stay near 85-90 % free; under about 70 % the clean 30 is the better picture), `[lat]` press to picture."),
    run("betweenold", "Free-run gate as before wave 9 (26.7 / 30 ms, 6 s cool-down, ladder by the 30 budget)", "tour",
        {"MSC_FPS_BETWEEN_ENTER": "26.7", "MSC_FPS_BETWEEN_LEAVE": "30", "MSC_FPS_BETWEEN_COOL": "6", "MSC_FPS_BETWEEN_COOLMAX": "96", "MSC_FPS_BETWEEN_FAIL": "30", "MSC_FPS_BETWEEN_OUT": "20", "MSC_DYNRES_PACED_UP_MS": "0"}, tier=1, group="pacing",
        why="The wave 9 plan: the free run (UP TO 60) is entered under 20 ms of the busiest stage, left over 25 ms for 60 frames, 45 s before the next try (doubled for a failed one), and the ladder judges a free run by 24 ms, so a scene of 20-30 ms keeps a clean 30 at full quality instead of a 1/2 mix at 36-46 pictures a second. This is the old gate. Against the default in a heavy stadium: `[fps] between:` lines (how often it switched, at what time), the `[pace]` display line (the share of 1 and 2 refresh holds in live play) and by eye whether the cadence is even; `[dynres]` levels while free."),
    run("smooth0", "Pictures drawn for their frame start (no show clock)", "tour", {"MSC_SMOOTH_CLOCK": "0"}, tier=2, group="pacing",
        why="MSC_SMOOTH_CLOCK=0 (the plan sets 1). With it on, under a held cadence each frame advances the game by its picture's predicted show-to-show time (whole refreshes, after 8 evenly held pictures), so a picture shown 2 refreshes shows 2 refreshes of motion even when its frame started a refresh early or late (r10: 8-41 % of evenly held pictures moved 1 or 3). Read the [smooth] lines of both runs: 'players+ball' mean / share over 0.5 must fall (expect about 0 in calm play), 'running players' and 'camera pan' |ratio-1| must fall; [speed] game time per real time, [present] hold mix, [lat] press to picture and [hitch] counts must be the same. By eye: a player running across the pitch at a held 30, the steady look of the 30."),
    run("smoothsteady3", "Show clock trusted after 3 evenly held pictures (not 8)", "worstgpu", {"MSC_SMOOTH_STEADY": "3"}, tier=2, group="pacing",
        why="MSC_SMOOTH_STEADY=3 (code default 8). The show clock (MSC_SMOOTH_CLOCK) only predicts a picture's show time after this many pictures in a row were held exactly the period; at The Dump (r11: 9.3 % of pictures held 3) a run of 8 is rare, so the clock is mostly idle there. Host model with the r10 statistics: pictures whose motion differs from their hold by more than half a refresh, heavy 24.8 % today, 21.2 % at 8, about 19 % at 3-4. Read [smooth] 'players+ball' share over 0.5 and the clock counters (given a show time, not steady) against the default run at The Dump."),
    run("tapcarry0", "No tap carry over a frame without a step", "tour", {"MSC_TAP_CARRY": "0", "MSC_SMOOTH_LOG": "0"}, tier=2, group="pacing",
        why="MSC_SMOOTH_LOG=0 rides on this run (the [smooth] line is a diagnostic with no effect beyond a log line per 300 pictures). MSC_TAP_CARRY=0 (the plan sets 1: a tap latched by a frame that ran no simulation step is carried to the next read). Only matters at 60 and in a free run (one frame in six has no step); a held 30 is the same either way. By hand: tap Square or Cross as short as you can in a 60 scene, 30 times each; count the ones that did nothing."),
    run("lat11", "1.1 cadence anchor and no handover limit", "tour", {"MSC_LAT_ANCHOR": "0", "MSC_LAT_DEPTH": "0"}, tier=2, group="pacing", why="CONSOLE-TESTS 1.2 row."),
    run("rules1", "1.1 dynamic resolution rules", "tour", {"MSC_DYNRES_RULES": "1", "MSC_DYNRES_UNDO": "0"}, tier=2, group="pacing",
        why="CONSOLE-TESTS 1.2 row (1.1 rules for the larger sizes), together with MSC_DYNRES_UNDO=0: the 1.2 controller undoes a step down that bought no GPU time (default on in the player build); here neither 1.2 rule is on, so the run is the whole of the 1.1 dynamic resolution."),
    run("max10", "Never larger than the game's own size", "tour", {"MSC_DYNRES_MAX": "1.0"}, tier=2, group="pacing", why="CONSOLE-TESTS row (Sharper When Possible OFF)."),
    run("blendnd0", "No-discard on blended alpha-tested draws turned off", "tour", {"MSC_VGX_BLENDNODISCARD": "0"}, tier=2, group="gpu",
        why="MSC_VGX_BLENDNODISCARD is on by default in the player build (blended, depth-write-off draws whose alpha test rejects alpha 0 only are made without the discard; identical pictures on the host). This run is the A/B: the old shaders."),
    run("cellclear0", "Atlas-cell clears back to one glClear each", "tour", {"MSC_VGX_CELLCLEAR": "0"}, tier=2, group="gpu",
        why="MSC_VGX_CELLCLEAR is on by default in the player build (the clears of the crowd-impostor atlas cells, about 27 scissored glClear calls a frame in open play, are made as one quad draw per run of cells; colour 0 and depth 1 to the bit, pictures equal on the host). This run is the A/B: one glClear per cell. Compare the frame time and the GPU time of open play."),
    run("atlate0", "Alpha-tested crowd draws left in game order", "tour", {"MSC_VGX_ATLATE": "0"}, tier=2, group="gpu",
        why="MSC_VGX_ATLATE=1 is on by default in 1.3 (13 Nov interim, review9: level 2 is proven on one stadium only): alpha-tested draws are held behind the opaque draws of their window so the tile GPU keeps hidden-surface removal for them. This run is the A/B: game order"),
    run("atlate2", "Alpha-tested draws sunk at level 2", "tour", {"MSC_VGX_ATLATE": "2"}, tier=2, group="gpu",
        why="MSC_VGX_ATLATE=2 (the crowd2b proposal; level 1 is the default since 13 Nov): judges level 2 on the console against the reference."),
    run("livetrades0", "Live-play trades all off", "tour", {"MSC_LIVE_TRADES": "0"}, tier=2, group="gpu",
        why="MSC_LIVE_TRADES=1 is the plan's default with the FULL picture policy (1.3, w8/trade.md): in live play only (open play, the game's TASK_GAMEPLAY) a player's shadow is one soft ellipse in the main scene and no shadow picture is drawn (no pass, no copy, no model copy); replays, cut-scenes, celebrations, Mega Strikes and menus keep the full pictures and the switch happens at the cut. This run turns the whole set off. Against the tour: GPU busy ms and the draws and scenes of open play (frame-timing, scene list), and look at the shadows at the cut from a cut-scene into play and back."),
    run("shadowfull", "Live-play shadows as pictures", "tour", {"MSC_SHADOW_LIVE": "full"}, tier=2, group="gpu",
        why="The shadow trade alone off (MSC_SHADOW_LIVE=full, the rest of MSC_LIVE_TRADES left as is): open play draws each player's shadow picture as the game does. Against the tour: the frame time of open play, and whether the flat ellipse reads as a shadow on every pitch."),
    run("shadowcut0", "Shadow cut spike unspread", "tour", {"MSC_SHADOW_CUT": "0"}, tier=2, group="gpu",
        why="MSC_SHADOW_CUT=3 is the default (1.3, w8/trade9.md): the first frames after live play (replay, celebration, Mega Strike) redraw at most 3 player shadow pictures a frame, those in view first, and show the flat shadow until each is ready, so no frame pays for all 11 pictures at once. This run is the A/B: 0 redraws every picture on the first frame outside live play, as before. Against the tour: the longest frame at the cut into a replay or celebration."),
    run("crowdlive1", "Live-play crowd at the 30 rate", "tour", {"MSC_CROWD_LIVE": "1"}, tier=2, group="gpu",
        why="MSC_CROWD_LIVE is OFF by default (12 Nov decision: at 60 pictures a second the near crowd refreshing 7.5 times a second animates in visible steps). This run turns it ON: under the gameplay camera the crowd sprites refresh at the 30 rate in time (8 sprite passes a frame instead of 16, about 25 fewer draws in open play, host census). Judge by eye: the near stands in open play, and the frame time against the reference"),
    run("crowdftb1", "Celebration crowd near to far", "tour", {"MSC_VGX_CROWDFTB": "1"}, tier=2, group="gpu",
        why="MSC_VGX_CROWDFTB is OFF in the plan since 13 Nov (interim, review9: one-pixel rim differences in celebrations, proven on one stadium). This run turns it on: when the crowd's fill passes 15 screens, as in a goal celebration, its draws and quads go near to far so the depth test rejects covered sprites (model: 25 to 31 percent fewer shaded crowd fragments). Judge the celebration crowd's look and the celebration frame time against the reference."),
    run("ctrim0", "Crowd sprite quads uncut", "tour", {"MSC_VGX_CROWDTRIM": "0"}, tier=2, group="gpu",
        why="MSC_VGX_CROWDTRIM is on by default in the player build (each crowd sprite quad is cut to the box the figure was drawn in; identical pictures on the host). This run is the A/B: whole quads, as before. Tier 1 has the look-route version (lookctrim0); this one is the tour for the frame timing over a whole match."),
    run("fe60off", "Front end cuts off", "tour", {"MSC_VGX_SKINDIRECT": "0", "MSC_VGX_SKINMEMO": "0", "MSC_VGX_BUILDFAST": "0", "MSC_VGX_FASTIDX": "0"}, tier=2, group="cpu",
        why="MSC_VGX_SKINDIRECT, SKINMEMO, BUILDFAST and FASTIDX are on by default in the player build (record-player thread about 11 % lighter in the goal celebration on the host; identical pictures). This run is the A/B: the old front end. Against the tour: the front end's ms in [frame-timing], mainly at a goal's start and in the celebration."),
    run("fe8off", "Front end memos off", "tour", {"MSC_VGX_EXTMEMO": "0", "MSC_VGX_DLWATCH": "0"}, tier=2, group="cpu",
        why="MSC_VGX_EXTMEMO and MSC_VGX_DLWATCH are on by default in 1.3 (the light-fact and wide-TEV-register answers of the shader key are kept until a register or light word they read changes; a cached display list is trusted while no write touched a page it uses, instead of summing its page generations on every hit; frame content hashes of a 5000-picture match and the det trace identical on the host, verify runs with no difference). This run is the A/B: both off. Against the tour: the front end's ms in [frame-timing], open play and the goal celebration."),
    run("physpose0", "Character poses back on the game thread (kill switch MSC_PHYS_POSE=0), split lines on", "tour", {"MSC_PHYS_POSE": "0", "MSC_GSPLIT": "1"}, tier=1, group="cpu",
        why="MSC_PHYS_POSE=1 is on in 1.3 (wave 9, physpar): the ten characters' poses of each simulation step (2.77 of the 4.83 ms 'physics' step of r11) run on the job lanes, committed in body order: the simulation is the same (host proof: det trace and 5204 frame hashes equal over a whole match60 and The Dump, mutants caught). This run is the A/B: against gsplit1-style default runs with MSC_GSPLIT=1 compare the [gsplit-phys] PostUpdate ms a step and the game column of [lane]; with the default the log has a [physpose] line (parallel= steps, lanes=3); here none. Game speed >= 98 percent, no new [hitch], [lat] p50 and p95 down or equal, or the default goes back to 0."),
    run("bpfull0", "Broad phase without the skips (MSC_ODE_BPFULL=0; r16 default 2), split lines on", "tour", {"MSC_ODE_BPFULL": "0", "MSC_GSPLIT": "2"}, tier=2, group="cpu",
        why="gt16b: MSC_ODE_BPFULL=2 is on in the r16 plan (flat pair walk skips the pairs collideAABBs returns from first, and steps over a run of same-body pairs; exact on match60, The Dump and Lava Pit on the host, mutants caught, verify wrong=0). This run is the off side: the broad phase as r15 (r11: 0.87 ms a step). Against the default run with MSC_GSPLIT=2: [gsplit-phys] broad phase ms a step."),
    run("bpfull1", "Broad phase with the skips but no run step-over (MSC_ODE_BPFULL=1; r16 default 2), split lines on", "tour", {"MSC_ODE_BPFULL": "1", "MSC_GSPLIT": "2"}, tier=2, group="cpu",
        why="gt16b: the skips without the run step-over, so the difference to the default (=2) is what the step-over adds. Read [gsplit-phys] broad phase ms a step."),
    run("bpverify", "Broad phase verify on the ARM path (MSC_ODE_BROADPHASE=2)", "tour", {"MSC_ODE_BROADPHASE": "2", "MSC_GSPLIT": "2"}, tier=2, group="cpu",
        why="gt16b: plain walk plus a check of every skip decision of the flat walk (X, Y, Z, category, body) against the float tests on the console's ARM/NEON code: every `ode broadphase: calls= pairs= skipped= wrong= left=` line must say wrong=0. Not a timing run."),
    run("physposev", "Character poses on the job lanes with the console verify (MSC_PHYS_POSE=2)", "tour", {"MSC_PHYS_POSE": "2", "MSC_GSPLIT": "1"}, tier=2, group="cpu",
        why="As physpose1, and every pose is also recomputed serially on the game thread from the saved state and compared bit for bit (slower: not a timing run). The [physpose] line every 300 steps must say mismatches=0 with checked= counting up; any other number means a lane computes something the game thread does not (floating-point mode, a shared scratch) and MSC_PHYS_POSE stays off."),
    run("gsplit1", "Game-thread split lines", "tour", {"MSC_GSPLIT": "1"}, tier=2, group="cpu",
        why="MSC_GSPLIT is a diagnostic of logging builds, off by default (not in the player build's plan). Every 300 pictures a [gsplit] line says where the game thread spent a picture: the simulation step's parts (ai, physics, pose), the render task's parts (characters, stadium, crowd, particles, hud), the frame send, waits (blocked, not CPU), and a 1 ms sampler of the phase the thread was in; sum against the thread's CPU time. The markers cost tens of clock reads a picture (about 0.1 ms on the console, estimate): compare the frame time with the tour before reading the split as the unobserved one."),
    run("snappar0", "Snapshot and crowd views back on the game thread", "tour", {"MSC_SNAP_PAR": "0", "MSC_CROWD_POS_PAR": "0", "MSC_GSPLIT": "1"}, tier=2, group="cpu",
        why="MSC_SNAP_PAR and MSC_CROWD_POS_PAR are on by default from wave 9: the ten characters' snapshot grab (every simulation step), their blend and the 150 power-up blends, and the crowd sprites' view matrices run on the job pool's three lanes (this parallel snapshot code came with 1.2 but was never compiled into the console build). Host proof: frame hashes and the det trace identical over 30000 pictures at Vice and The Dump's whole route, proof mutants caught. This run is the A/B: both off, with the split lines. Against gsplit1 (same route, defaults, split lines): [gsplit] blend and crowd-pos ms, game-thread ms in [lane], [jobs] parallel/busy/caller-waited, [snappar] lines in gsplit1. Keep on only if the game thread is lighter and nothing in the owner's acceptance line gets worse."),
    run("renderrng1", "Render random streams, frame-lock countdown and blend made by the simulation task (MSC_RENDER_RNG=1, MSC_FRAMELOCK_STEP=1, MSC_BLEND_STEP=1)", "tour", {"MSC_RENDER_RNG": "1", "MSC_FRAMELOCK_STEP": "1", "MSC_BLEND_STEP": "1"}, tier=2, group="cpu",
        why="r15: OFF by default (Fable batch 2); this is the ON side. MSC_RENDER_RNG, MSC_FRAMELOCK_STEP and MSC_BLEND_STEP (simthread10; MSC_VIS_STEP stays off): the simulation steps draw from their own copies of nlDefaultSeed and gEffectsRandomSeed, and the frame-lock countdown that ends a goal or kick-off hold is made by the simulation task instead of the drawing task. Nothing the player sees changes except the cosmetic random sequence (crowd jitter, sound volume and pitch, particle spread). Host proof: the det trace is identical with the drawing skipped on odd pictures or run twice (it moved at step 161 before), and moves again with both switches off. This run is the A/B: both off. With the default the log has [renderrng] and [framelock] lines; here the [renderrng] line says 0/0 and no [framelock] line. No timing difference is expected (a few loads per step): any difference in [lane] game ms or game speed means the switch is not the cause, rerun."),
    run("ctrect0", "Crowd trim cell set by rectangle off", "tour", {"MSC_VGX_CROWDTRIM_RECT": "0"}, tier=2, group="cpu",
        why="MSC_VGX_CROWDTRIM_RECT is on in the 1.3 plan (febe24: exact on the host over 2800 pictures, verify 0 differ). This run is the A/B: off. Against the tour: the back-end thread's ms in the walk-out, cut-scenes and replays (CtScan was about 15 % of the host back-end instructions there)."),
    run("fe8scanoff", "Shadow ledger cut off", "tour", {"MSC_VGX_LEDGERFAST": "0"}, tier=2, group="cpu",
        why="MSC_VGX_LEDGERFAST is on by default in 1.3 (the shadow quad's alpha ledger projects each vertex once per draw, skips a polygon the bands already hold and a clip that cannot cut; the vertex bytes and shadow rectangles the back end's scans produce are identical on the host over 5000 pictures, verify runs with no difference). This run is the A/B: off. Against the tour: the back-end thread's ms (the GL thread, core 1 with the frame send) in open play and the goal celebration."),
    run("ctrimplay1", "Crowd trim in open play too", "tour", {"MSC_VGX_CROWDTRIM_PLAY": "1"}, tier=2, group="cpu",
        why="MSC_VGX_CROWDTRIM_PLAY=0 is the default in 1.3: the crowd trim does not follow the impostor passes and does not cut the crowd in match-play pictures (the crowd is far and small there: the host census at Vice and The Dump has the trim saving 0.1 to 0.9 of a screen of fill in open play, against 1.2 M instructions of the back-end core); it still acts in walk-outs, replays and cut-scenes. This run is the A/B: the trim in open play too, as in 1.2. Against the tour: the back-end thread's ms in open play (the CPU the trim costs) and the GPU ms (what it saves)."),
    run("recoff", "Recorder cuts off", "tour", {"MSC_REC_FAST": "0"}, tier=2, group="cpu",
        why="MSC_REC_FAST is on by default in 1.3 (the game-side recorder writes registers inline, and keeps the fog register words of the last call; the recorded stream is byte-identical on the host over 5000 pictures at Vice and The Dump). This run is the A/B: off. Against the tour: the frame-send worker's ms (core 1 with the GL back end) in open play and the goal celebration."),
    run("crowdvisoff", "Crowd figure lists off", "tour", {"MSC_CROWD_VISFAST": "0"}, tier=2, group="cpu",
        why="MSC_CROWD_VISFAST is on by default in the player build (the crowd's per-picture visibility pass appends precomputed figure lists; the same lists in the same order, det trace and frame hashes identical on the host). This run is the A/B: the game's loop over every figure. Against the tour: game-thread ms in the goal celebration and replays."),
    run("warboff", "Heat shimmer cuts off (r16 warble19 defaults)", "nlava", {"MSC_VGX_CLEARVEH": "0", "MSC_VGX_EFBSWAP_NEXT": "0", "MSC_VGX_PASSISO_COVER": "0", "MSC_WARBLE_LITE": "0"}, tier=2, group="draw",
        why="The heat shimmer at Lava Pit and Sand Tomb runs with 2 scenes fewer by default (exact on the host: 0 pixels differ) and, in the 60 mode, its map every second picture (owner-approved cheaper look). This run is the A/B at Lava Pit: [lane] gpu and late % against nlava; any wrong picture at the shimmer = report."),
    run("poseoff", "Pose jobs and lazy NPC evaluate off", "tour", {"MSC_POSE_JOBS": "0", "MSC_NPC_EVAL_LAZY": "0", "MSC_HEAD_STAT": "1", "MSC_GSPLIT": "2"}, tier=2, group="cpu",
        why="MSC_POSE_JOBS=1 and MSC_NPC_EVAL_LAZY=1 are on by default in the player build (w8/crowd24.md: the crowd sprites' poses are made on the job lanes before the game thread builds their packets; an NPC the render filter hides is not evaluated). Exact on the host: det trace and all frame hashes identical to off at Vice and The Dump. This run is the A/B: the old serial path. Against the tour: [gsplit] crowd-sprites (was 1.2-1.3 ms), the stadium stage (the NPC evaluates) and the render task, and the [jobs] line (calls, wake latency)."),
    run("crowdgov0", "Crowd governor off (MSC_CROWD_GOV=0)", "tour", {"MSC_CROWD_GOV": "0", "MSC_HEAD_STAT": "1", "MSC_GSPLIT": "2"}, tier=2, group="cpu",
        why="MSC_CROWD_GOV=1 is on by default in the player build (w8/crowd25.md; owner 17 Nov: cheaper crowd animation is fine when needed): in live play, while the game thread is the limiting lane and over 87 % of the period, the crowd sprites refresh less often by size class (far first, six levels, the deepest a very slow idle), and come back when there is room. Render only (det trace identical on the host). This run is the A/B: the full crowd always. Against the tour: the [crowdgov] lines (level changes, seconds at each level), [gsplit] crowd-sprites, [head] play with / without a step."),
    run("crowdgtoff", "Crowd game-thread cuts off", "tour", {"MSC_CROWD_GT": "0"}, tier=2, group="cpu",
        why="MSC_CROWD_GT=5 is on by default in the player build (w8/gt10.md: the crowd sprites' view matrices are made where a sprite is captured and not for every sprite on every picture (bit 1); no view for a captured sprite with nothing to show (bit 4)). Host: det trace and every frame hash identical to MSC_CROWD_GT=0. This run is the A/B: the old path. Against the tour: the [gsplit] crowd-pos / crowd-sprites ms in open play (r11 had 0.63 and 1.14 at Vice). Bit 2 (the sprites of one texture captured on the same picture, one pose per refresh) is a separate opt-in run, crowdgtpose1."),
    run("aiavoid0", "AvoidController early outs off (MSC_AI_AVOID=0; r16 default 3)", "tour", {"MSC_AI_AVOID": "0", "MSC_GSPLIT": "2"}, tier=2, group="cpu",
        why="MSC_AI_AVOID=3 is on by default in the player build (w8/gt10.md): CanAddAvoidance runs its pure tests before the AVL lookup (bit 1) and the base IsWithinRange rejects a pair that is surely out of range by squared distance before the emulated square root (bit 2). Exact: det trace and frame hashes identical on the host. This run is the A/B: the old code. Against the tour: [gsplit-ai] ai ms a step (r11: 1.71 at Vice). MSC_AI_AVOID=7 adds a verify (the [aiavoid] line at exit: verify wrong must be 0)."),
    run("cadenceoff", "Cadence off", "tour", {"MSC_CADENCE": "0", "MSC_PRESENT_SYNC_START": "0", "MSC_FX_ATTACH": "0", "MSC_CADENCE_FULL": "0"}, tier=2, group="cpu",
        why="MSC_CADENCE, MSC_PRESENT_SYNC_START and MSC_FX_ATTACH are on by default in the player build (replays and cut-scenes held at a locked 30 from their cut; picture alignment starting at 3 vblanks; the live ball trail drawn on the drawn ball). This run is the A/B: the old behaviour. Against the tour: the [cadence] lines at each cut, whether the rate inside a replay or the goal celebration is steady, and the open play after it back at its own rate; [present] L over the first 10 s."),
    run("aalast0", "Full picture: the samples go first, then the size (the earlier order)", "tour", {"MSC_PICTURE_HOLD_AALAST": "0"}, tier=2, group="picture",
        why="hold9, owner's decision 13 Nov (full resolution without anti-aliasing looks worse than a smaller picture with it). Against d1 (the new order: 960x544 -> 912x520 -> 864x488 -> 800x456 with 2x, one sample only at the bottom): the picture's size line in [hold] and the GPU / fps at Vice, Lava Pit and The Dump. By eye in the same stadiums: the edges at the lower sizes with 2x against full size with one sample."),
    run("start0", "Full picture: matches start at full size (no safe start)", "tour", {"MSC_PICTURE_HOLD_START": "0"}, tier=2, group="picture",
        why="hold10 (r10 console run). Against d1: the [hold] match starts line (start step and class) and the first 20 s of fps at The Dump, Lava Pit and the others that started at 15-20 fps; the A/B of the safe start."),
    run("scenes0", "Full picture: replays and cut-scenes jump to the held size", "tour", {"MSC_PICTURE_HOLD_SCENES": "0"}, tier=2, group="picture",
        why="hold10. Against d1: the fps of held scenes (cut-scenes, celebrations, replays) at The Dump, 19-21 fps at the held size in r10."),
    run("margin1", "Full picture: a held 30 budgets the whole period (33.3 ms)", "tour", {"MSC_PICTURE_HOLD_BUDGET_MARGIN": "1.0"}, tier=2, group="picture",
        why="hold10. Against d1 (80 per cent of the period, 26.7 ms): the [hitch] count, fps windows and [pace] lines in a held 30 (r10 had 24.8-27 fps windows at The Dump with the whole period)."),
    run("scenepol1", "Full picture: held scenes get their own policy (hold12, off by default in r15)", "tour", {"MSC_PICTURE_HOLD_SCENEPOL": "1"}, tier=2, group="picture",
        why="hold12, r15 A/B. Against d1: the goal cut-scenes and replays ([frame-timing] p50 and the [hold] held scene lines): r11/r12 ran 14-20 pictures a second with GPU 37-71 ms; with the scene policy they start at a level known to fit."),
    run("scenestart", "Full picture: every held scene starts at 704x400 one sample", "tour", {"MSC_PICTURE_HOLD_SCENE_START": "10"}, tier=2, group="picture",
        why="hold12. Against d1: the cost of the lowest scene level and what the policy climbs to ([hold] held scene step lines)."),
    run("pace0", "Full picture: the budget stays 16.7 ms, the old slow climb back", "tour", {"MSC_PICTURE_HOLD_PACE": "0"}, tier=2, group="picture",
        why="hold9 (13 Nov console r7: at Vice the picture was shed to 704x400 for no gain, 44-47 fps either way). Against d1: [hold] lines (steps down in a CPU-limited match must be 0 with PACE on), fps at Vice, The Dump's size under a held 30."),
    run("holdgpubusy0", "Full picture: every late picture counts towards a size step (the old rule)", "tour", {"MSC_PICTURE_HOLD_GPUBUSY": "0"}, tier=2, group="picture",
        why="holdgpu24 (s27d1b: 'GPU mean 26.9 ms of 26.5' stepped Vice 960x544 -> 912x520 -> 864x488 while [lane] gpu read 16.4 ms). Against d1 (GPUBUSY on): the [hold] step lines and 'rule' text, the GPUBUSY 30 s line (late pictures not counted), sizes over the match."),
    run("stickyoff", "Smooth 60: the old 60 / 30 switching (no sticky hold)", "tour", {"MSC_LOCK60": "1", "MSC_LOCK60_STICKY": "0"}, tier=2, group="picture",
        why="holdgpu24 (s29: 4-8 switches between 60 and the held 30 every 30 s). Against a Smooth 60 run with the default sticky hold: the [head] 'relief: policy 30 now, N of M pictures at 30, K switches' lines and [fps] switch lines."),
    run("floors0", "Picture floors off: the old ladders down to 704x400 and 448x320", "tour", {"MSC_PICTURE_FLOORS": "0"}, tier=2, group="picture",
        why="holdgpu24 (owner 17 Nov: nothing below 864x488 in Quality, 800x456 in Smooth 60). Against d1 only for the cost of the floors: fps and [hold] sizes at The Dump (a diagnostic of the OLD ladder, below the floor: not for play)."),
    run("commit0", "Full picture: targets may be let go and made again in a match", "tour", {"MSC_PICTURE_HOLD_COMMIT": "0"}, tier=2, group="picture",
        why="hold9. Against d1: no '[hold] render target ... was let go' and no 'made in play' line in a match with COMMIT on; the '[hold] kick-off' lines say what the match has."),
    run("holdoff", "Full picture off: the older size ladder", "tour", {"MSC_PICTURE_HOLD": "0"}, tier=2, group="picture",
        why="MSC_PICTURE_HOLD=full (renderer/src/vgx_hold.h) is the player build's default picture for live play: 960x544 held, a late frame gives up the effects target, far shadows, all shadows and the crowd refresh in that order, then up to three size steps (864x488, 776x440, 704x400; targets made at match load, about 8 MiB); frozen in replays, cut-scenes and Mega Strikes. This run is the A/B: the older ladder (the game's size, larger or smaller by the load, 4x and 2x multisampling when there is room). Against the tour: [frame-timing] p50 and p99, late frames, fps in open play, and the [hold] lines of the tour (steps, the match summary with the seconds at each level, objects made after a size change, the video memory of the targets made ahead)."),
    run("holdfixed", "Full picture fixed, no shedding", "tour", {"MSC_PICTURE_HOLD": "full", "MSC_PICTURE_HOLD_MAXSTEP": "0"}, tier=2, group="picture",
        why="The second comparison for the full picture: 960x544 held with every step turned off (no shedding, no size step: what a plain MSC_VITA_RENDER=960x544 does, plus the freeze logic that does nothing here). Against the tour and holdoff: how much of the late-frame count the shedding and the size steps remove; the [hold] 30 s lines show the share of frames over the 60 budget with nothing shed."),
    run("polishoff", "Sharpening ease, HD fade and still tolerance off", "tour", {"MSC_SHARPEN_EASE": "0", "MSC_HDTEX_FADE": "0", "MSC_STILL_TOL": "0"}, tier=2, group="smooth",
        why="MSC_SHARPEN_EASE=20 (sharpening strength eased over 20 pictures at a size step), MSC_HDTEX_FADE=8 (an HD texture that arrives late is sampled one level coarser and eases in over 8 pictures) and MSC_STILL_TOL=1 (only with STILL BACKDROPS on) are the defaults; this run is the old behaviour. By eye: a size step and a character's first seconds."),
    run("dlkeepoff", "Display lists let go after 10 s", "tour", {"MSC_DL_KEEP": "0"}, tier=2, group="cpu",
        why="MSC_DL_KEEP=24 is on by default in the player build (the lists of the walk-out, the celebration and the replay stay while they add up to 24 MiB, so a goal builds almost none: host, goals 2 and 3 built 135 and 18 lists in one picture instead of 187 and 161 with a 12 MiB cap, before the cap was sized from the measured 19 MiB). This run is the A/B: the old sweep after 600 frames. Against the tour: the front-end ms of the picture after a goal ([frame-timing] worst), the [dlkeep] lines, and the free memory over a long match."),
    run("keepoff", "Cut-scene files read from the card in play", "tour", {"MSC_DVD_KEEP_KIB": "0", "MSC_NIS_PREFETCH": "0", "MSC_VITA_STALL_GUARD": "0"}, tier=2, group="smooth",
        why="MSC_DVD_KEEP_KIB=8192 is on by default in the player build (the goal, Mega Strike and end-of-game cut-scene files, the characters' animations and the defence HUD are read ahead while the walk-out plays and answered from memory after kick-off; 4 MiB of the C heap) and MSC_NIS_PREFETCH / MSC_VITA_STALL_GUARD are on in the code. This run is the A/B: every such file read from the card when the goal or Mega Strike asks. Against the tour: the [hitch] lines at the goal and the Mega Strike cuts (cause wait or upstream), [dvdtrace] / [load] lines with MSC_DVD_TRACE, the heap figures."),
    run("pretouchoff", "Textures uploaded at their first draw", "tour", {"MSC_TEX_PRETOUCH": "0"}, tier=2, group="smooth",
        why="MSC_TEX_PRETOUCH=2 is on by default in the player build: the textures a match has loaded (and those a cut-scene's file registers later) are uploaded by the renderer a few a frame while the match loads and in play ahead of their first draw, and kept against the 32 MiB texture budget until the front end is back (host estimate: 31.5 MB held at the end of a match against 20.0 MB without, no upload after kick-off against 92). This run is the A/B: every texture uploaded at its first draw. Against the tour: the [hitch] lines at kick-off, the goals and the Mega Strikes (tex n/K), the texture memory of the [vram] lines, whether any scene shows a texture late."),
    run("fxtight", "Tight particle polygons, Pipeline Central", "pipe", {"MSC_FX_TIGHT": "8", "MSC_FX_TIGHT_DROP": "1", "MSC_FX_TIGHT_STAT": "300"}, tier=2, group="stadium",
        why="Same picture, fewer transparent pixels. Against pipe1 and pipe2: GPU ms of [frame-timing]; the [fxtight] lines give the sprites and the screens saved by count."),
    run("fxtightf", "Tight particle polygons with a floor on faint texels, Pipeline Central", "pipe", {"MSC_FX_TIGHT": "8", "MSC_FX_TIGHT_DROP": "1", "MSC_FX_TIGHT_FLOOR": "3", "MSC_FX_TIGHT_STAT": "300"}, tier=2, group="stadium",
        why="A lossy step beyond fxtight, about half the mist's pixels for 7 % of its light. Against fxtight: GPU ms, and look at the mist's edges."),
    run("fxgov", "Fill governor: sprites thin before the picture shrinks, Pipeline Central", "pipe", {"MSC_FX_GOV": "1", "MSC_FX_TIGHT_STAT": "300"}, tier=2, group="stadium",
        why="Against pipe1 and pipe2: fewer [vgx] dynamic resolution steps, a larger mean render scale, [vgx] fill governor lines; look at the mist for grain."),
    # 1.3 FX (full resolution): the picture fixed at 960x544, where there is no ladder; each cut against its own 960 reference of the same route.
    run("mega960", "960x544 fixed, Mega Save (reference for the fill cuts at full size)", "mega", {"MSC_VITA_RENDER": "960x544", "MSC_FX_STAT": "1"}, tier=2, group="fill960",
        why="The Mega Save at the owner's target size. [frame-timing] GPU ms and fps; [fxstat] says whether the effects pass ran (it should: the Mega hint)."),
    run("fxmaxh960", "Effects target uncapped (A/B of the 1.3 default 224), 960x544 Mega Save", "mega", {"MSC_VITA_RENDER": "960x544", "MSC_FX_STAT": "1", "MSC_FX_LOWRES_MAXH": "0"}, tier=2, group="fill960",
        why="The effects pass keeps the pixel count it has at the game's own size (395x224 instead of 480x272: 0.68 of the pixels). Against mega960: GPU ms; look at the sparks for softness."),
    run("fxthin960", "Mega haze thinned at any size, 960x544 Mega Save", "mega", {"MSC_VITA_RENDER": "960x544", "MSC_FX_STAT": "1", "MSC_FX_THIN": "1"}, tier=2, group="fill960",
        why="At a fixed size the Mega picture never reaches its smallest step, so MSC_FX_THIN's default never thins. Against mega960: GPU ms; look at the haze."),
    run("fxpass0960", "Effects in the scene with tight sprites instead of the reduced pass, 960x544 Mega Save", "mega", {"MSC_VITA_RENDER": "960x544", "MSC_FX_STAT": "1", "MSC_FX_LOWRES": "0", "MSC_FX_TIGHT": "8", "MSC_FX_TIGHT_DROP": "1", "MSC_FX_TIGHT_STAT": "300"}, tier=2, group="fill960",
        why="At 960x544 each run of the effects pass stores and reloads the whole picture and its depth (about 9 MB of traffic, an estimate), which may cost more than the pixels it saves; Galaxy found its GPU cost followed scene changes and draws. Against mega960: GPU ms. If this wins, the pass should not be the default at full size."),
    run("fxgovmega960", "Fill governor in Mega Strikes too, 960x544 Mega Save", "mega", {"MSC_VITA_RENDER": "960x544", "MSC_FX_STAT": "1", "MSC_FX_GOV": "1", "MSC_FX_GOV_MEGA": "1", "MSC_FX_TIGHT_STAT": "300"}, tier=2, group="fill960",
        why="Mega Strike effects are the largest fill of the game (host census: 12.9 screens of additive sprites a frame before the effects pass). Large sprites thin to one in two or four while the GPU is over budget. Against mega960: GPU ms, [vgx] fill governor (fixed size) lines; look at the glow for grain."),
    run("pipe960", "960x544 fixed, Pipeline Central (reference)", "pipe", {"MSC_VITA_RENDER": "960x544", "MSC_FX_STAT": "1"}, tier=2, group="fill960",
        why="Open play at the owner's target size with the 1.3 defaults (the heavy-stadium profile of Pipeline included)."),
    run("fxgov960", "Fill governor at a fixed 960x544, Pipeline Central", "pipe", {"MSC_VITA_RENDER": "960x544", "MSC_FX_STAT": "1", "MSC_FX_GOV": "1", "MSC_FX_TIGHT_STAT": "300"}, tier=2, group="fill960",
        why="MSC_FX_GOV_FIXED (default 1) lets the governor act without a ladder. Against pipe960: [vgx] fill governor (fixed size) lines, GPU ms, late frames; look at the mist for grain."),
    run("fxplay960", "Effects pass in open play by pixels, 960x544, Pipeline Central", "pipe", {"MSC_VITA_RENDER": "960x544", "MSC_FX_STAT": "1", "MSC_FX_LOWRES_SCENES": "play", "MSC_FX_LOWRES_LEVEL": "3", "MSC_FX_LOWRES_MINPX": "300000"}, tier=2, group="fill960",
        why="Runs of effects over about a screen go to the reduced target in open play at a fixed size too (replays take the same line with nis,play or all). Against pipe960: GPU ms; [fxstat] runs; look at the mist and the ball's glow."),
    # 1.3 LOCKED 60 (MSC_LOCK60): the 60 fps policy with a per-class ladder floor; each against the default of the same route.
    run("lock60pipe", "LOCKED 60, Pipeline Central (open play)", "pipe", {"MSC_LOCK60": "1", "MSC_FX_STAT": "1"}, tier=2, group="lock60",
        why="Against pipe1 and pipe2. Read [fps] LOCKED 60 (policy 60 from the start), [lane] (game, send, back end, GPU ms), [lock60] step down/step up lines (levels, size, reason, GPU ms), [lock60] scene lines (preset, floor, ready rungs), [lock60] 30 s lines, [frame-timing] p50 and the late-frame count "
            "(frames over 16.7 ms). Pass: p50 16.7, late under 2 %, no step line in a calm stretch longer than 10 s, no [lock60] line saying a step made objects (the one that begins `the step to level`). By eye: the pitch and players at the play floor (0.70)."),
    run("lock60tour", "LOCKED 60, tour (menus, play, Mega Strike, replay)", "tour", {"MSC_LOCK60": "1", "MSC_FX_STAT": "1"}, tier=2, group="lock60",
        why="Against d1 and dmid. Read the same lines per scene class: [lock60] scene and `ended` lines carry the class (menu, play, replay, cut, mega, heavy) with its measured frames and share over the 60 budget. Menus and replays may relieve to 30 (a [fps] lock60 relief line "
            "that ends with the scene); open play must not. Late-frame counts per class against d1."),
    run("lock60mega", "LOCKED 60, Mega Save", "mega", {"MSC_LOCK60": "1", "MSC_FX_STAT": "1"}, tier=2, group="lock60",
        why="Against megad1. The unthinned Mega Strike is the worst case of the model: expect the effects governor to thin first, then rungs to the mega floor (0.56), then a relief line. "
            "Read the order of the [lock60] and [vgx] fill governor lines, GPU ms and late frames."),
    run("lock60replay", "LOCKED 60, replays", "replay", {"MSC_LOCK60": "1", "MSC_FX_STAT": "1"}, tier=2, group="lock60",
        why="Replay is the model's expensive class (extra scenes). Read whether the policy holds 60 or relieves to 30 ([fps] auto: 60 -> 30, [lock60] `no ready rung` or `steps after the floor` lines) and that it returns to 60 when play resumes; late frames per class."),
    run("lock60lava", "LOCKED 60, Lava Pit (open play)", "clava", {"MSC_LOCK60": "1", "MSC_FX_STAT": "1"}, tier=2, group="lock60",
        why="Owner 14 Nov 20:42 (can a lower graphics setting give 60?): r7 Lava Pit GPU was 24-25 ms even at 704x400. Expect the ladder to reach its play floor and the policy to relieve to 30; read the [lock60] step and relief lines, GPU ms, late frames."),
    run("lock60dump", "LOCKED 60, The Dump (open play)", "worstgpu", {"MSC_LOCK60": "1", "MSC_FX_STAT": "1"}, tier=2, group="lock60",
        why="Owner 14 Nov 20:42: The Dump GPU was 19-23 ms at 704x400 on r7. The heaviest case: does LOCKED 60 relieve to a steady 30 cleanly (no flapping), and how far is the floor from 16.7 ms."),
    run("lock60pix", "LOCKED 60 with the pixel share raised to 0.45, Pipeline Central", "pipe", {"MSC_LOCK60": "1", "MSC_LOCK60_PIXSHARE": "0.45", "MSC_FX_STAT": "1"}, tier=2, group="lock60",
        why="The governor's cost model assumes 30 % of the GPU time follows the pixel count. Against lock60pipe: if the steps overshoot or undershoot (GPU ms after a step against the predicted), this says which way the share is wrong."),
    run("gpubusy0", "GPU busy time raw (A/B of the 1.3 default: the display scene's wait left out), Pipeline Central", "pipe", {"MSC_GPU_BUSY_FIX": "0", "MSC_VITA_RT_LOG": "120"}, tier=2, group="lock60",
        why="Against pipe1: with the fix off the ladder reads busy_us as before. In the [rt] lines busy near the frame period (16.7 ms at 60, 33.4 at 30) with short 3D scenes means the display scene's wait was counted; the [vgx-vita] GPU busy time line says which figure was read. Compare [vgx] dynamic resolution steps and the mean scale: fewer steps down with the fix on is the proof."),
    run("between30", "AUTO holding 30 (A/B of the 1.3 default MSC_FPS_BETWEEN=pace), Pipeline Central", "pipe", {"MSC_FPS_BETWEEN": "30"}, tier=2, group="pacing",
        why="Against pipe1 (AUTO now rates between 30 and 60 in a match). Read [fps] auto lines (how long it holds 30), [frame-timing] p50 and the share of pictures at 60, 30 and between, and [present] (the cadence: 1-2-3 vblank mix versus 2-2-2)."),
    run("crowdsort", "Crowd sprites drawn near to far (8 depth bands)", "tour", {"MSC_VGX_CROWDSORT": "8"}, tier=2, group="picture",
        why="Picture-changing crowd option (soft sprite rims only, 0.5 to 0.7% of the pixels): the depth test rejects covered sprites before they are shaded. The host model says about 6% fewer shaded crowd fragments for about 100 more draws."),
    run("crowdprez", "Crowd depth pre-pass", "tour", {"MSC_VGX_CROWDPREZ": "1"}, tier=2, group="picture",
        why="Alpha-tested depth-only pass, then the colour pass. The model says it saves nothing on a tile-based GPU (the alpha test is paid twice); run for completeness."),
] + [
    # ---- r15 consolidated sitting (wave 10; sitting.py orders them, r16-sitting.txt is the list): the runs the r15 branches asked for that had no kit id ----
    run("crep1", "r15 defaults: goals, celebrations, replays with the goalie close-up (texture census, skipped-draw counters)", "crep", tier=2, group="r15",
        why="The owner's 'light textures on goals' symptom: [texcensus] lines (light10) and the warm10 'draws left out for a program not ready' counters in the goalie close-up replay, plus the replay by eye. Same route as cur-rep1."),
] + [
    run("dis-%s-%s" % (side, n), "Discarded frames %s (MSC_DISCARD_FAST=%s), The Dump, run %s" % ("paced fast" if side == "on" else "paced at one retrace each", "1, the default" if side == "on" else "0", n),
        "worstgpu", {} if side == "on" else {"MSC_DISCARD_FAST": "0"}, tier=2, group="r15",
        why="stall10 A/B, three a side alternating (dis-on, dis-off, dis-off, dis-on, dis-on, dis-off): the [discard] lines (time spent in discarded frames at the stadium load, estimate 3520 -> 1545 ms) and the [hitch] gaps at the load; against each other, not against worstgpu1.")
    for n in "abc" for side in ("on", "off")
] + [
    run(i, "%s (run %d: acts from the second session)" % (t, n), "tour", e, tier=2, group="r15",
        why="Second launch of %s (the first is the recording run: the card holds the list only after a session ended). Same read as the first run; the [cuts] match line must show the pass objects made ahead / pinned." % b)
    for b, t, e, n, i in (("cutsshapes1", "Pass objects of the cuts made before kick-off (shapes ON)", {"MSC_CUTS_SHAPES": "1"}, 2, "cutsshapes1-2"),
                          ("scenepin1", "Pass objects of a match kept for the match (pin ON)", {"MSC_POOL_SCENE_PIN": "1"}, 2, "scenepin1-2"),
                          ("warm1", "Other sample mode's program variants drawn at load (warm-up ON)", {"MSC_FXAA_WARM": "1"}, 2, "warm1-2"))
] + [
    run("dump-all", "All three cuts switches together at The Dump (shapes + pin + warm-up), recording run", "worstgpu",
        {"MSC_CUTS_SHAPES": "1", "MSC_POOL_SCENE_PIN": "1", "MSC_FXAA_WARM": "1"}, tier=2, group="r15",
        why="cuts10: the three switches together at the worst stadium. First launch records, the second (dump-all-2) acts: the [cuts] lines (first uses after kick-off, pins held), [hitch] at the goal cuts, [mem] FAILED counts."),
    run("dump-all-2", "All three cuts switches together at The Dump, second session (acts)", "worstgpu",
        {"MSC_CUTS_SHAPES": "1", "MSC_POOL_SCENE_PIN": "1", "MSC_FXAA_WARM": "1"}, tier=2, group="r15", why="Second launch of dump-all: the lists are on the card now."),
    run("ram112-a", "Video memory pool asked at 112 MiB (MSC_VITA_VGL_RAM_MIB=112), Vice", "tour", {"MSC_VITA_VGL_RAM_MIB": "112"}, tier=2, group="r15",
        why="mem10: the vitaGL RAM pool 112 MiB. Read the [mem] FAILED counts (186-235 in r11), pool free, crash or not."),
    run("ram112-dump", "Video memory pool asked at 112 MiB, The Dump", "worstgpu", {"MSC_VITA_VGL_RAM_MIB": "112"}, tier=2, group="r15", why="As ram112-a at the heaviest stadium."),
    run("circ24", "vitaGL command ring 24 MiB (MSC_VITA_VGL_CIRC_MIB=24), The Dump, with the per-frame profile", "worstgpu", {"MSC_VITA_VGL_CIRC_MIB": "24", "MSC_VGX_PROF": "3"}, tier=2, group="r15",
        why="mem10: the ring 32 -> 24 MiB frees 8 MiB; MSC_VGX_PROF=3 adds the [vgx] per frame lines. Read [mem] FAILED counts and that no frame waits on the ring."),
    run("la2-on", "One byte a texel for I4/I8 textures (MSC_VGX_LA88=2), Vice", "tour", {"MSC_VGX_LA88": "2"}, tier=2, group="r15",
        why="mem10: the existing LA88 switch, never run on a console; host pictures identical, upload bytes -75 %. Read the texture census and upload time and look for wrong greys."),
] + [
    # ---- r16 (wave 11): the new switches' console runs. The run lines come from the helpers' notes (w8/<helper>.md); the minutes are the route's, the helpers' own figures are in their notes. ----
    run("aiavoid7", "AvoidController with the verify bit (MSC_AI_AVOID=7), split lines on", "tour", {"MSC_AI_AVOID": "7", "MSC_GSPLIT": "2"}, tier=2, group="r16",
        why="gt16b: verify of bit 2 on the ARM path: the [aiavoid] line must say verify wrong 0. Only meaningful with items out (a Lava or power-up route); not a timing run."),
    run("snap0", "Snapshot with the full copy as r15 (MSC_SNAP_ONECOPY=0; r16 plan default 1), split lines on", "tour", {"MSC_SNAP_ONECOPY": "0", "MSC_GSPLIT": "2"}, tier=2, group="r16",
        why="gt16a: the ON side (the plan holds the switch at 0 until its nudge mutant and the seed break are shown caught). Skips the dead m_PrevNodeMatrices copy; exact on match60 (host). Read [gsplit] snapshot ms a step against d1 with MSC_GSPLIT=2."),
    run("rneon1", "Render-side matrix code on NEON (MSC_RENDER_NEON=1), split lines on", "tour", {"MSC_RENDER_NEON": "1", "MSC_GSPLIT": "2"}, tier=2, group="r16",
        why="gt16a: OFF by default. [gsplit] render task ms a step against the default; game speed >= 98 percent, no new [hitch]."),
    run("rneonv", "Render-side matrix code on NEON with the verify (MSC_RENDER_NEON=2), split lines on", "tour", {"MSC_RENDER_NEON": "2", "MSC_GSPLIT": "2"}, tier=2, group="r16",
        why="gt16a: every NEON result is compared with the scalar one; the mismatch count must stay 0 (emulator: 541,408 calls, 0 mismatches). Not a timing run."),
    run("nodepth1", "Pass objects made without depth memory (MSC_VGX_NODEPTH_PASS=1), The Dump", "worstgpu", {"MSC_VGX_NODEPTH_PASS": "1"}, tier=2, group="r16",
        why="vgl16 (vitaGL 0038), the one run judged FIRST: pass objects that only copy or draw passes without a depth plane get no depth/stencil memory (est. about 58 MiB of depth not allocated over the run; emulator: pool 0 FAILED 114 -> 17). Console: any fault or corrupted picture (partial render) means the switch is never used. Else read depth KiB, free memory, [mem] FAILED counts and the [bet] line."),
    run("nodepth1v", "Pass objects made without depth memory (MSC_VGX_NODEPTH_PASS=1), Vice", "tour", {"MSC_VGX_NODEPTH_PASS": "1"}, tier=2, group="r16",
        why="vgl16: the same at Vice. Only after nodepth1 ran without a fault."),
    run("rtpool1", "Render-target pool of 8 (MSC_VGX_RT_POOL=8), The Dump", "worstgpu", {"MSC_VGX_RT_POOL": "8"}, tier=2, group="r16",
        why="vgl16 (vitaGL 0039): render targets of a size left over are reused instead of deleted and made again. Read the [rtpool] reuse lines and the [hitch] target ms; [mem] FAILED must not rise."),
    run("megartpool1", "Render-target pool of 8 (MSC_VGX_RT_POOL=8), Mega Save", "mega", {"MSC_VGX_RT_POOL": "8"}, tier=2, group="r16",
        why="vgl16: the Mega cut-in pass objects."),
    run("heldaa13", "Replays and cut-scenes as r15 (MSC_PICTURE_HOLD_MSAA_HELD=auto: 2x at Vice), Vice", "tour", {"MSC_PICTURE_HOLD_MSAA_HELD": "auto"}, tier=2, group="r16",
        why="Owner 14 Nov 16:55: held scenes one sample everywhere is the r16 default. This is the r15 side: Vice replays and cut-scenes at 2x. Against d1: held-scene fps, [fxaa] compositor acted counts, and the swap cost at the first cut ([cuts] first use)."),
    run("heldahead0", "One-sample held targets made at the cut as before (MSC_HELD_AA_AHEAD=0), Vice", "tour", {"MSC_HELD_AA_AHEAD": "0"}, tier=3, group="r16",
        why="r16 default 1 makes the one-sample held-scene target ahead outside play (Fable verify7). Off side: the first goal cut, replay or walk-out makes it at the cut; read [fxaa] one-sample held target lines, [cuts] targets at the first held cut and [mem] FAILED."),
    run("heldaa13w2", "One-sample held scenes (r16 default) with the paced program warm-up (MSC_FXAA_WARM=2), Vice", "tour", {"MSC_FXAA_WARM": "2"}, tier=2, group="r16",
        why="held16: the paced warm. [cuts] first use at the first held cut must show +0 fragment variants +0 targets; FAILED counts and `[fxaa] warm stopped` are read. Run twice: the list is on the card from the second session."),
    run("heldaa13w2-2", "As heldaa13w2, second session (acts)", "tour", {"MSC_FXAA_WARM": "2"}, tier=2, group="r16",
        why="Second launch of heldaa13w2: the lists are on the card now."),
    run("heldaa13w1", "One-sample held scenes (r16 default) with the old warm-up (MSC_FXAA_WARM=1), Vice", "tour", {"MSC_FXAA_WARM": "1"}, tier=2, group="r16",
        why="held16: the w10 warm (it crashed the r9 build at boot) for comparison; only after heldaa13w2 booted."),
    run("scenepol1s", "Held-scene policy with size sets (MSC_PICTURE_HOLD_SCENEPOL=1, MSC_COPY_SIZESETS=1)", "tour", {"MSC_PICTURE_HOLD_SCENEPOL": "1", "MSC_COPY_SIZESETS": "1"}, tier=2, group="r16",
        why="held16: copy textures of the old size are kept for the way back: `cpy new/rs` at the second replay should fall; read the [sizesets] lines."),
    run("heldscn3", "Held scenes: bits 1 and 2 (MSC_HELD_SCENES=3), Vice", "tour", {"MSC_HELD_SCENES": "3"}, tier=2, group="r16",
        why="scenes16: render-only, held scenes only. Bit 1 no depth load/store on scenes whose depth is never read, bit 2 the pass memo and run merge in held scenes. Offscreen scenes per held picture and front/back end ms in held scenes (14 -> about 8 scenes is the aim, estimate); pictures must look the same."),
    run("heldscn7", "Held scenes: bits 1, 2 and 4 (MSC_HELD_SCENES=7), Vice", "tour", {"MSC_HELD_SCENES": "7"}, tier=2, group="r16",
        why="scenes16: bit 4 adds the half-size passes in held scenes (visible: softer glow)."),
    run("cad3", "Steady 3-refresh cadence in held scenes (MSC_HELD_CADENCE=auto), Vice", "tour", {"MSC_HELD_CADENCE": "auto"}, tier=2, group="r16",
        why="cadence16: does a steady 3 in goal cut-scenes and replays beat the 3/4/2 mix? Read the [cad3] lines, [pace] held 1/2/3+ in the held windows (wanted 3 at 100 percent where work is under 50 ms) and the [fps] MSC_HELD_CADENCE summary."),
    run("cad3d", "Steady 3-refresh cadence in held scenes (MSC_HELD_CADENCE=auto), The Dump", "worstgpu", {"MSC_HELD_CADENCE": "auto"}, tier=2, group="r16",
        why="cadence16: the same at the heavy stadium, where the scenes miss most."),
] + [
    run("dp16%s-%s" % (side, n), "The Dump start step: %s, run %s" % ("seed off (MSC_DUMP_START=0, the r15 start at step 9)" if side == "a" else "seed on (the r16 default)", n),
        "worstgpu", {"MSC_DUMP_START": "0"} if side == "a" else {}, tier=2, group="r16",
        why="dumpplay16, three a side alternating (dp16a, dp16b, dp16b, dp16a, ...): a = MSC_DUMP_START=0 (start step 9, 752x424), b = the default (the first match at The Dump starts on the bottom picture step 704x400). b: `[hold] match starts ... starting at step 10`, no `step 9 -> 10` line; late pictures in the first two windows (16-27 before, est. 14-25 fewer). a: the kick-off windows and the 9 -> 10 move and its objects.")
    for n in "abc" for side in ("a", "b")
] + [
    # ---- exact17 (14 Nov, Galaxy's finding: five exact front-end switches sat at 0 for days because nobody ran them on a console). The switches that are OFF in the r16 plan, exact on the host or with a
    # verify mode, and not yet on a console. Existing runs of the others: snap1, rneon1 / rneonv, la2-on, lai8, prefetch15, clusterlazy, discardblend0, texmemo2 (notes: w8/exact17.md). ----
    run("gxd13v", "Front-end draw merge by state serial (MSC_VGX_GXD bit 13 on) with every shortcut verified (bit 30), Vice", "tour", {"MSC_VGX_GXD": "2147450879"}, tier=2, group="exact",
        why="exact17: GXD_MERGESER (Galaxy GXV_MERGEFAST) is OFF (+0.01 percent instructions on the host's armhf count: no gain expected), exact by construction. Bit 30 (GXD_VERIFY) runs every front-end shortcut and the plain code and reports a disagreement: the count at exit must be 0 (the first 8 are logged). Not a timing run; the front-end ms against d1 only says what the verify costs."),
    run("gxd15v", "Front-end state resolution that leaves early (MSC_VGX_GXD bit 15 on) with every shortcut verified (bit 30), Vice", "tour", {"MSC_VGX_GXD": "2147475455"}, tier=2, group="exact",
        why="exact17: GXD_RSFAST (Galaxy e90cc10) is OFF (+0.15 percent instructions on the host: no gain expected). The verify count must be 0, as in gxd13v."),
    run("hsrproven1", "Blended draws proven opaque drawn without the blend (MSC_VGX_HSR_PROVEN=1), Vice", "tour", {"MSC_VGX_HSR_PROVEN": "1", "MSC_VGX_HSR_COUNT": "1"}, tier=2, group="exact",
        why="exact17: OFF, exact (the draw's blend function only, no shader text change, so the pack is unaffected). No verify mode: the [hsrcount] line says how many blended draws the key's numbers prove (alpha 1). By eye: no missing or opaque-looking smoke, glow or shadow. The GPU ms and the late pictures against d1 say whether hidden-surface removal pays."),
    run("hsrfacts1", "As hsrproven1 plus the texture and vertex-colour facts (MSC_VGX_HSR_FACTS=1), Vice", "tour", {"MSC_VGX_HSR_PROVEN": "1", "MSC_VGX_HSR_FACTS": "1", "MSC_VGX_HSR_COUNT": "1"}, tier=2, group="exact",
        why="exact17: FACTS adds to the proof (textures with all texels opaque, vertex colour from the material and ambient registers). [hsrcount] must show more proven draws than hsrproven1; same picture by eye."),
    run("ctrect1v", "Crowd trim cell set by rectangle with the verify (MSC_VGX_CROWDTRIM_RECT=2), Vice", "tour", {"MSC_VGX_CROWDTRIM_RECT": "2"}, tier=2, group="exact",
        why="exact17: the plan has it on (ctrect0 is the off A/B); this runs the plain form on a copy and compares all cells and the grid: no [ctrect] MISMATCH line in a goal celebration. Not a timing run."),
    run("exactall-vice", "Every exact OFF switch on, with the verify modes, Vice", "tour", {"MSC_VGX_GXD": "2147483647", "MSC_VGX_HSR_PROVEN": "1", "MSC_VGX_HSR_FACTS": "1", "MSC_VGX_HSR_COUNT": "1", "MSC_VGX_CROWDTRIM_RECT": "2", "MSC_RENDER_NEON": "1", "MSC_VGX_LA88": "2", "MSC_PREFETCH": "15", "MSC_GSPLIT": "2"}, tier=2, group="exact",
        why="exact17: the combined run (Galaxy's five-switch pattern): MSC_VGX_GXD all bits and the verify bit 30, HSR_PROVEN + FACTS (+ COUNT), CROWDTRIM_RECT=2 (verify), RENDER_NEON=1 (boot self-test), LA88=2, PREFETCH=15, SNAP_ONECOPY=1, split lines on. Left out: DISCARD_BLEND (new shader texts, the pack must be seeded first), RT_POOL and NODEPTH_PASS (memory, own runs). Read: every verify count 0, no new FAILED, [lane] ms against d1 (the verify costs a little front-end time: strip bit 30 for a pure timing run)."),
    run("exactall-dump", "Every exact OFF switch on, with the verify modes, The Dump", "worstgpu", {"MSC_VGX_GXD": "2147483647", "MSC_VGX_HSR_PROVEN": "1", "MSC_VGX_HSR_FACTS": "1", "MSC_VGX_HSR_COUNT": "1", "MSC_VGX_CROWDTRIM_RECT": "2", "MSC_RENDER_NEON": "1", "MSC_VGX_LA88": "2", "MSC_PREFETCH": "15", "MSC_GSPLIT": "2"}, tier=2, group="exact",
        why="exact17: the same at The Dump (the GPU-bound stadium): GPU ms and late pictures against worstgpu1."),
    run("dlweld0", "Display-list vertex weld off (MSC_VGX_DLWELD=0; plan default 1 since r17), The Dump", "worstgpu", {"MSC_VGX_DLWELD": "0"}, tier=2, group="exact",
        why="weld17 (port of Galaxy GXV_DLWELD, exact: a pure index remap at the list promotion), ON in the plan since r17; this is its off side: GPU ms and late pictures of worstgpu1 (weld on) against this run (off); keep it on unless the on side is slower, faults, or a [hitch] appears in the warm match; the [dlweld] line gives lists, vertices before/after, KiB saved and the ms spent welding (load cost). Galaxy measured the drawn turn 23.9 -> 23.2 ms in the hub (console figure, Galaxy)."),
    run("dlrefresh0", "Display-list refresh off (MSC_VGX_DLREFRESH=0; plan default 1 since dlrefresh24), the walk-out at Vice", "tour", {"MSC_VGX_DLREFRESH": "0"}, tier=2, group="exact",
        why="dlrefresh24 (exact on the host: per-picture back-end hashes, frame hashes and det trace identical, mutant caught), ON in the plan: the characters' skinned display lists that the game rewrites before every draw (8-11 a picture in the walk-out) were decoded from scratch at every call (the bypass window); now only the arrays that changed are decoded again. This is its off side: the walk-out's front-end ms (the [lane-fe] / [late] lines of the @begin walkout window) and the p50 of the walk-out against the reference run d1 (on); the [dl] / [lane-dl] line must show refreshed above 0 in d1 and 0 here. Keep it on unless the on side is slower, faults, or a character mesh looks wrong in the walk-out."),
    run("fepar0", "Front-end decode worker off (MSC_VGX_FEPAR=0; plan default 1 since fepar25), the lane lines at Vice", "tour", {"MSC_VGX_FEPAR": "0", "MSC_VGX_FE_SPLIT": "1"}, tier=2, group="exact",
        why="fepar25 (exact on the host: per-picture back-end hashes, frame hashes and det trace identical off / on / verify at Vice goal + replay + walk-out, The Dump and the tour; join-skipped mutant changes the hash, verify mutant reported), ON in the plan: the vertex decode (and the CPU transform) of plain stream draws runs on a worker thread on core 1 and the player joins it at the end of the picture. This is its off side: the front-end lane (fe in the [lane] line, decode= in the [lane-fe] line, FE_SPLIT is on) in live play against the on side (d1 / any tour run with MSC_VGX_FE_SPLIT=1), the game thread and core 1 load; the [fepar] line of the on side gives jobs, vertices, worker busy and join wait per picture. Expected: fe about 2 ms lower in play (estimate from the decode scope 1.9-2.7 ms and the soft-transform scope 0.3 ms of the s28i-fe lanes, less the overhead); keep it on unless the on side is slower, faults, or a [hitch] appears."),
    run("fepar0d", "Front-end decode worker off (MSC_VGX_FEPAR=0), The Dump", "worstgpu", {"MSC_VGX_FEPAR": "0", "MSC_VGX_FE_SPLIT": "1"}, tier=2, group="exact",
        why="fepar25: the same off side at The Dump (the lane lines in play against worstgpu1 with MSC_VGX_FE_SPLIT=1)."),
    run("fe26off", "State-resolution cuts off (MSC_VGX_TEXSAME=0 MSC_VGX_MTXSPLIT=0; plan default 1 since fe26), the lane lines at Vice", "tour", {"MSC_VGX_TEXSAME": "0", "MSC_VGX_MTXSPLIT": "0", "MSC_VGX_FE_SPLIT": "1", "MSC_HEAD_STAT": "1"}, tier=2, group="exact",
        why="fe26 (exact on the host: per-picture back-end hashes, frame hashes and det trace identical off / on / verify at the tour, The Dump and Vice goal + replay + walk-out; mutants caught), ON in the plan: a repeated texture bind leaves the bindings clean and the matrix blocks are rebuilt from the rows written, so more draws take ResolveState's early exit. This is its off side: resolve-state= and list-hit= of the [lane-fe] line in play windows against the on side (any tour run with MSC_VGX_FE_SPLIT=1), alternating on/off pairs. Expected: resolve-state 0.2-0.4 ms lower (estimate: x86 callgrind -8.5 % of ResolveState); keep it on unless the on side is slower, faults, or a texture or a model looks wrong."),
    run("stdio0", "Log printing through newlib's FILE as in 1.2 (MSC_STDIO_LOCK=0; plan default 1 since crash26b), Vice Smooth 60", "tour", {"MSC_STDIO_LOCK": "0", "MSC_LOCK60": "1", "MSC_HEAD_STAT": "1", "MSC_GSPLIT": "2", "MSC_FX_STAT": "1"}, tier=3, group="exact",
        why="crash26b: the off side of the log printing that goes around stdout / stderr's FILE, for an A/B of a timing cost only. WARNING: this side can crash (the 18 Nov dumps: two threads printing at once made newlib copy (size_t)-1 bytes over the heap; host repro w8/crash26/repro fails in every run). Expected: no difference in the [head] / [lane] lines, the same log text."),
    run("freeguard0", "Referenced-only free guard off (MSC_VGX_FREE_GUARD_REF=0; plan default 3 since stab13), Vice", "tour", {"MSC_VGX_FREE_GUARD_REF": "0", "MSC_HEAD_STAT": "1"}, tier=3, group="exact",
        why="stab13 (vitaGL 0050): the off side of the guard that holds only a delete of a block a scene drew into within the last four presents. On a run that never makes such a delete the two sides are identical (host model, 5 mutants). Expected: no [free-guard] FREE WHILE REFERENCED line on the on side, the same [head] lines."),
] + [
    run("c17base", "Start-up shader warm-up capped at 300 programs as r15 (MSC_WARM_CAP=300; r16 default 400), Vice", "tour", {"MSC_WARM_CAP": "300"}, tier=2, group="r16",
        why="comp17: r16 links the whole packaged list (329 programs) under the loading picture instead of 300. Against d1: [boot] present 2 time (the cost) and [cuts] first-use programs at the first goal, replay and walk-out (the gain)."),
    run("l17d", "Back-pressure takes the pad sleep off its room (MSC_LAT_BP_SLEEP=1), Vice", "tour", {"MSC_LAT_BP_SLEEP": "1"}, tier=2, group="r16",
        why="lessons17 (Galaxy lesson 1): the back-pressure gate counted the pad sleep that follows it as free room. Read the back-pressure waited share, the [pace] 3+ hold share (must not rise) and press p50."),
    run("l17a", "Background at 30 pair with the late-picture causes, Vice", "tour", {"MSC_BG30": "2", "MSC_BG30_BALANCE": "3", "MSC_PAIR_RATE": "1", "MSC_FPS": "30", "MSC_GSPLIT": "1", "MSC_RENDER_RNG": "1", "MSC_FRAMELOCK_STEP": "1", "MSC_VIS_STEP": "1"}, tier=3, group="r16",
        why="lessons17 (Galaxy lesson 3): the [late] line charges each late picture to GPU, game CPU, render thread or front end in the bg30 pair; decides bg30 cuts against game-thread cuts."),
    run("l17b", "Background at 30 pair with the fast leave (MSC_PAIR_FAST_LEAVE=3), Vice", "tour", {"MSC_BG30": "2", "MSC_BG30_BALANCE": "3", "MSC_PAIR_RATE": "1", "MSC_FPS": "30", "MSC_GSPLIT": "1", "MSC_RENDER_RNG": "1", "MSC_FRAMELOCK_STEP": "1", "MSC_VIS_STEP": "1", "MSC_PAIR_FAST_LEAVE": "3"}, tier=3, group="r16",
        why="lessons17 (Galaxy: off after 3 of 16 pairs over 31 ms): does the pair leave sooner on a scene that half fits; [pair] flips, [late] share, evenness."),
    run("l17c", "Background at 30 pair with the screen guards (MSC_BG30_SCREENS=1, MSC_PAIR_SCREENS=1), Vice", "tour", {"MSC_BG30": "2", "MSC_BG30_BALANCE": "3", "MSC_PAIR_RATE": "1", "MSC_FPS": "30", "MSC_GSPLIT": "1", "MSC_RENDER_RNG": "1", "MSC_FRAMELOCK_STEP": "1", "MSC_VIS_STEP": "1", "MSC_BG30_SCREENS": "1", "MSC_PAIR_SCREENS": "1"}, tier=3, group="r16",
        why="lessons17 (Galaxy's Game Over flicker): frame generation off on game-over, state-change, wipe, pause and home-menu screens; the counts print with the switches off too."),
    run("lat16a", "Press-to-picture baseline: pad age log (MSC_LAT_PADAGE=1)", "tour", {"MSC_LAT_PADAGE": "1"}, tier=2, group="r16",
        why="press16: baseline `[lat16]` lines: the sample age (lower bound), the controller period, the room fed in (entry -> shown; expect 30-50 ms against the old 16.7)."),
    run("lat16b", "Press-to-picture: pad sampled late (MSC_LAT_SAMPLE=1, MSC_LAT_PADAGE=1)", "tour", {"MSC_LAT_SAMPLE": "1", "MSC_LAT_PADAGE": "1"}, tier=2, group="r16",
        why="press16: the cut. Finds %, arrival ms and waits; summ.py press p50/p95; the [pace] 3+ hold share must not rise; [present] judder."),
    run("lat16c", "Press-to-picture: the late latch with the true room (MSC_LAT_SLACK_LB=1)", "tour", {"MSC_LAT_SLACK_LB": "1"}, tier=2, group="r16",
        why="press16: the existing late latch with the true room: `[fps] late latch delayed` share and mean against the 0-9 percent of today, press, late percent."),
    run("lat16d", "Press-to-picture at the Dump (MSC_LAT_SAMPLE=1, MSC_LAT_PADAGE=1)", "worstgpu", {"MSC_LAT_SAMPLE": "1", "MSC_LAT_PADAGE": "1"}, tier=2, group="r16",
        why="press16: the same as lat16b at The Dump (smaller room)."),
    run("lat16e", "Press-to-picture: sample margin 2000 us (MSC_LAT_SAMPLE=1, MSC_LAT_SAMPLE_MARGIN_US=2000)", "tour", {"MSC_LAT_SAMPLE": "1", "MSC_LAT_SAMPLE_MARGIN_US": "2000"}, tier=2, group="r16",
        why="press16: the margin trial, only if lat16b shows no rise in late pictures."),
    run("popin1", "Materials made whole at the walk-out (MSC_POPIN_BLOCK=1)", "tour", {"MSC_POPIN_BLOCK": "1"}, tier=2, group="r16",
        why="popin16: the `[popin]` line (window draws made whole, links made now, would-have-waited count); by eye: a material missing at the walk-out."),
] + [
    run("cutlearn%s%s%s" % (b, "d" if d else "", "" if n == 1 else "-2"), "Cut learning (MSC_CUT_LEARN=%s), %s%s" % (b, "The Dump" if d else "Vice", "" if n == 1 else ", second session (acts)"),
        "worstgpu" if d else "tour", {"MSC_CUT_LEARN": b}, tier=2, group="r16",
        why="learn16: what the first launch uses at a goal cut is written per stadium beside the shader list and made ahead at the next match (bit 1 textures, bit 2 programs and variants made as degenerate triangles before kick-off, 4 ms a present, 12 MiB video-memory floor, at most 96). %s Read `[cutlearn]` (list loaded, made, linked new, not made, floor), `[cuts] match` totals and `first use` lines at the goal cuts (emulator: 23 -> 1), the [hitch] at the cuts and [mem] FAILED." % ("Second launch: the list is on the card." if n == 2 else "First launch records the list."))
    for b, d in (("1", False), ("2", False), ("3", True)) for n in (1, 2)
] + [
    run("pgo-%s-%s-%s" % (side, scn, n), "PGO A/B: %s eboot at %s, run %s" % ("r15 (no PGO)" if side == "base" else "r15 PGO", "Vice" if scn == "d1" else "The Dump", n),
        "tour" if scn == "d1" else "worstgpu", tier=2, group="r15",
        why="pgo10/pgo15: the same env as %s, only the eboot differs (session.sh eswap installs the executable alone). Judge by the sim time, [fps] slip, [hitch] counts and [lat] p50/p95; watch the first 60 s for a crash (a PGO build once exposed a crash 25-45 s in)." % ("d1" if scn == "d1" else "worstgpu1"))
    for scn in ("d1", "wg") for side in ("base", "on") for n in "abc"
] + [
    run(i, "Pacing %s: %s, %s" % (step, label, "Vice" if scn == "d1" else "The Dump"), "tour" if scn == "d1" else "worstgpu", e, tier=2, group="r15",
        why="pace10 (r15-integration.md): %s. Deciding lines: [lat] pad->shown p50/p95 (down or equal over 3 runs), [pace] late pictures (must not rise; 2.4 %% Vice, 9.3 %% Dump in r11), [fps] avg >= 29.4 and 'phase sync on ... delayed N' (N=0 = the switch did not act), [speed] >= 98 %%, [hitch] not higher. Model (estimate): -1..+1 ms." % label)
    for scn, ids in (("d1", ("v1c", "v2l", "v4p", "v5h")), ("wg", ("d1c", "d2l", "d4p", "d5h")))
    for k, (step, label, e) in enumerate(zip(ids, ("today's AUTO", "even 30 held (MSC_FPS=30)", "even 30 + phase sync (MSC_PHASE_SYNC=1)", "phase sync + hold budget margin 0.90"),
                                             ({}, {"MSC_FPS": "30"}, {"MSC_FPS": "30", "MSC_PHASE_SYNC": "1"}, {"MSC_FPS": "30", "MSC_PHASE_SYNC": "1", "MSC_PICTURE_HOLD_BUDGET_MARGIN": "0.90"})))
    for i in ["%s%d" % (step, n) for n in (1, 2, 3)]
]

# r16 goal-scenario runs (route16): fast-boot routes with the test-only `score` command (a goal, celebration and replay in the first minute). They exist only when the route
# files are in the tree (route16 merged); sitting.py turns the slot live by itself once the coordinator marks route16 merged and safe (a Lava emulator fault after `score` is being investigated).
_GS = [("gsvice16", "gsvice", "fb-vice-goal", "Vice"), ("gsdump16", "gsdump", "fb-dump-goal", "The Dump"), ("gslava16", "gslava", "fb-lava-goal", "Lava Pit")]
for _id, _route, _fb, _name in _GS:
    if (Path(__file__).resolve().parent / "fastboot" / "routes" / (_fb + ".txt")).is_file():
        RUNS.append(run(_id, "Goal scenario at %s (route %s): goal, celebration and replay in the first minute, split lines on" % (_name, _fb), _route, {"MSC_GSPLIT": "2"}, tier=2, group="r16",
                        why="route16 + logs16: the test-only `score` command puts a goal into the first minute, so the held scenes (celebration, replay) are measured with the held-scene hitch budget (MSC_HITCH_LINES=1500; [hitch] lines of the held class after picture 3000) and the game-thread split lines. Fast boot only."))

# ======================================================================================================================================================
# r17 (kit20, 15 Nov): the runs of the wave 17-20 helpers for ONE consolidated sitting (sitting.py orders them; CONSOLE-PLAN-r17.md has the rules). Lines come from the
# helpers' notes (plan-wave17, plan-simthread17 section 5, overlap18, head17, split17, link17, memsite17, price17, probe18, galaxyport18, live19, warble19, census19, dumpgpu20).
# Every run's env names the switch under test, so the log's `[vita] np-player.txt: KEY=VALUE` line proves it was in force (INVALID if it is missing).
# ======================================================================================================================================================
def _m(*ds):
    out = {}
    for d in ds:
        out.update(d)
    return out


GS60 = {"MSC_LOCK60": "1", "MSC_HEAD_STAT": "1", "MSC_GSPLIT": "2", "MSC_FX_STAT": "1"}        # head17 + split lines on the 60 mode
AI27OFF = {"MSC_AI27_SORT": "0", "MSC_AI27_VCOPY": "0", "MSC_AI27_AVOID": "0", "MSC_AI27_CTRL": "0"}   # ai27: the OFF side
SIM26OFF = {"MSC_SIM26_BP": "0", "MSC_SIM26_BONES": "0", "MSC_SIM26_POSEORDER": "0", "MSC_SIM26_NETGRAB": "0", "MSC_SIM26_ISLANDS": "0"}   # sim26: the OFF side
PHYS27OFF = {"MSC_PHYS27_PLANE": "0", "MSC_PHYS27_DIV": "0"}   # phys27: the OFF side
SIMT = {"MSC_SIMT": "2", "MSC_RENDER_RNG": "2", "MSC_OVERLAP_LOG": "1"}                       # overlap18; MSC_OVERLAP_LOG=1 prints the `[fs] np_sim` line (Fable verify10 P5)
SIMT_NEEDS = " NEEDS THE simfix20 PACKAGE (lost wake-up fix, Fable verify10 P1). INVALID without the `[fs] np_sim` line (MSC_OVERLAP_LOG=1: `[simt] final` needs atexit, which the console exit does not run). MSC_SIMT never goes into the Smooth table from this sitting."
CUTS3 = {"MSC_VGX_CLEARVEH": "1", "MSC_VGX_EFBSWAP_NEXT": "1", "MSC_VGX_PASSISO_COVER": "1"}


def _r17(id, title, route, env, why, tier=2, group="r17", **kw):
    return run(id, title, route, env, tier=tier, group=group, why=why, **kw)


RUNS += [
    # ---- block A: the 60 answer, then the step on its own thread ----
    _r17("lock60gs-v", "LOCKED 60 with the headroom and split lines, Vice tour", "tour", GS60,
         "head17 + split17: the pass lines of the 60 mode. Read `[head]` (game-thread busy p50 / p95 / max per scene class, with and without a step, render-thread p95, GPU p95, pictures over 16.7 ms by cause), `[gsplit]`, `[split] held == finished, leaked 0`. Ship rule: p50 16.7, late under 2 %, no relief in open play, game-thread p95 under 14.5 ms. Check that the render-lane samples are not 0.0 (they read 0.0 in the emulator) and that the with-step column has samples (the emulator had 0 steps in the window)."),
    _r17("lock60gs-d", "LOCKED 60 with the headroom and split lines, The Dump", "worstgpu", GS60,
         "As lock60gs-v at the heaviest stadium: the answer is expected to be a steady 30 by relief; read how far the GPU p95 is from 16.7 and whether the relief flaps (a 60 try every 32 s)."),
    # sim26 (w8/sim26.md): the five exact simulation cuts are ON in the plan; their OFF side at both stadiums (alternate with lock60gs-v / lock60gs-d, two pairs each) and the ARM verify
    _r17("sim26off-v", "Simulation cuts of sim26 off (MSC_SIM26_*=0), LOCKED 60 Vice with split lines", "tour", _m(GS60, SIM26OFF),
         "sim26: the OFF side of MSC_SIM26_BP / BONES / POSEORDER / NETGRAB / ISLANDS (all exact: host det trace and frame hashes equal on five routes). Alternate with lock60gs-v (ON), at least two pairs. Read [gsplit] sim ms a picture (ON expected lower; host ESTIMATE in w8/sim26.md), [gsplit-phys] broad, postupdate, islands / qs-*, [head] game thread with a step p50 / p95."),
    _r17("sim26off-d", "Simulation cuts of sim26 off (MSC_SIM26_*=0), LOCKED 60 The Dump with split lines", "worstgpu", _m(GS60, SIM26OFF),
         "As sim26off-v at The Dump; alternate with lock60gs-d, at least two pairs."),
    # ai27 (w8/ai27.md): four exact AI cuts are ON in the plan; their OFF side at both stadiums (alternate with lock60gs-v / lock60gs-d, two pairs each) and the ARM verify
    _r17("ai27off-v", "AI cuts of ai27 off (MSC_AI27_*=0), LOCKED 60 Vice with split lines", "tour", _m(GS60, AI27OFF),
         "ai27: the OFF side of MSC_AI27_SORT / VCOPY / AVOID / CTRL (exact: host det trace and frame hashes equal on five routes). Alternate with lock60gs-v (ON), at least two pairs. Read [gsplit-ai] ai ms per STEP (ON expected about 0.1-0.2 ms lower; host ESTIMATE in w8/ai27.md: -53 k of 1.62 M instr a step) and its vm-states / script lines."),
    _r17("ai27off-d", "AI cuts of ai27 off (MSC_AI27_*=0), LOCKED 60 The Dump with split lines", "worstgpu", _m(GS60, AI27OFF),
         "As ai27off-v at The Dump; alternate with lock60gs-d, at least two pairs."),
    _r17("ai27ver", "ai27 verify modes on the console's ARM code (MSC_AI27_SORT/AVOID/CTRL=2)", "tour", _m(GS60, {"MSC_AI27_SORT": "2", "MSC_AI27_AVOID": "2", "MSC_AI27_CTRL": "2"}),
         "ai27: every [ai27] line must say wrong 0 with its counts going up (SORT proves that the Vita's qsort also leaves an in-order list alone); INVALID if a line is missing. Not a timing run."),
    # gt27 (w8/gt27.md): the asynchronous snapshot blend (default off): alternate with lock60gs-v / lock60gs-d, two pairs each
    _r17("gt27ba0-v", "Snapshot blend back on the game thread (MSC_BLEND_ASYNC=0; plan default 1 since 18 Nov), LOCKED 60 Vice with split lines", "tour", _m(GS60, {"MSC_BLEND_ASYNC": "0"}),
         "gt27: the characters' and power-ups' blend kicked onto the helper lane at the top of RenderFrame, joined in RenderSnapshot::Blend (exact on the host). Alternate with lock60gs-v (OFF), two pairs. Read [gsplit] render-task blend (ON expected ~1.0 -> ~0.2 ms if the lane is done in time) and wait-other in [gsplit-rest] (the join wait), [gsplit-pic] 1-step p50 / p95, [blendasync] taken / made again (made again must stay near 0), [lane] waits and stalls (stalls must be 0), [jobs] crowd fork waits, [head] play with a step."),
    _r17("gt27ba0-d", "Snapshot blend back on the game thread (MSC_BLEND_ASYNC=0; plan default 1 since 18 Nov), LOCKED 60 The Dump with split lines", "worstgpu", _m(GS60, {"MSC_BLEND_ASYNC": "0"}),
         "As gt27ba-v at The Dump; alternate with lock60gs-d, two pairs."),
    _r17("gt27bap-v", "Snapshot blend on the helper lane with the crowd stages before the join (MSC_BLEND_ASYNC=1 MSC_SIMT_ORDER=P), LOCKED 60 Vice", "tour", _m(GS60, {"MSC_SIMT_ORDER": "P"}),
         "gt27: as gt27ba-v, with the crowd stages moved before the blend join (order P: the crowd stands differ by a picture of animation, not exact): the larger overlap. Against gt27ba-v: render-task, wait-other, [gsplit-pic] 1-step p50 / p95. Look at the stands for anything odd."),
    _r17("gt27pa-v", "Per-core probe on its own thread (MSC_FPS_PROBE_ASYNC=1), LOCKED 60 Vice with split lines", "tour", _m(GS60, {"MSC_FPS_PROBE_ASYNC": "1", "MSC_GSPLIT_DETAIL": "0"}),
         "gt27: ON side (code default 0; Vita-only code, no host proof possible): [gsplit-rest] begin-frame probe ~0.6 ms -> ~0; the [fps] 'probe async' line (stale readings near 0); switch decisions and 60 share against lock60gs-v with MSC_GSPLIT_DETAIL=0; [gsplit-pic] 1-step p50 / p95 (net)."),
    _r17("refreshpf0", "Packet refresh without prefetch (MSC_FS_REFRESH_PREFETCH=0), LOCKED 60 Vice with split lines", "tour", _m(GS60, {"MSC_FS_REFRESH_PREFETCH": "0", "MSC_GSPLIT_DETAIL": "0"}),
         "gt27: the OFF side of MSC_FS_REFRESH_PREFETCH=1 (plan, exact): [gsplit-rest] end-frame pkt-refresh against lock60gs-v run with MSC_GSPLIT_DETAIL=0."),
    _r17("sim26ver", "sim26 verify modes on the console's ARM code (MSC_SIM26_BP/BONES/NETGRAB/ISLANDS=2)", "tour", _m(GS60, {"MSC_SIM26_BP": "2", "MSC_SIM26_BONES": "2", "MSC_SIM26_NETGRAB": "2", "MSC_SIM26_ISLANDS": "2"}),
         "sim26: every [sim26bp] / [sim26bones] / [sim26net] / [sim26isl] line must say wrong 0 with its counts going up; INVALID if a line is missing. Not a timing run (the verify modes do the work twice)."),
    # phys27 (w8/phys27.md): the two exact cuts are ON in the plan; their OFF side at both stadiums (alternate with lock60gs-v / lock60gs-d, two pairs each) and the ARM verify
    _r17("phys27off-v", "Physics cuts of phys27 off (MSC_PHYS27_*=0), LOCKED 60 Vice with split lines", "tour", _m(GS60, PHYS27OFF),
         "phys27: the OFF side of MSC_PHYS27_PLANE / DIV (exact: host det trace and frame hashes equal on five routes). Alternate with lock60gs-v (ON), at least two pairs. Read [gsplit-phys] near ms a step (ON expected about 0.1-0.15 lower: the plane pairs are not run; counts `callbacks` fall from about 85 to about 15), postupdate (DIV, small), [gsplit] physics ms a step, [head] game thread with a step p50 / p95. ESTIMATE of the gain: 0.15-0.25 ms a step."),
    _r17("phys27off-d", "Physics cuts of phys27 off (MSC_PHYS27_*=0), LOCKED 60 The Dump with split lines", "worstgpu", _m(GS60, PHYS27OFF),
         "As phys27off-v at The Dump; alternate with lock60gs-d, at least two pairs."),
    _r17("phys27ver", "phys27 verify modes on the console's ARM code (MSC_PHYS27_PLANE=2, MSC_PHYS27_DIV=2)", "tour", _m(GS60, {"MSC_PHYS27_PLANE": "2", "MSC_PHYS27_DIV": "2"}),
         "phys27: the [phys27plane] and [phys27div] lines must say wrong 0 with their counts going up; INVALID if a line is missing. Not a timing run."),
    _r17("simt-blk-v", "Match step on its own thread, the game thread blocking (MSC_SIMT=1), LOCKED 60 Vice", "tour", _m(GS60, {"MSC_SIMT": "1", "MSC_OVERLAP_LOG": "1"}),
         "block17: FIRST simt run and the gate for all the others: boots, plays; `[simt]` stack high-water, host-heap 0, GX 0; game busy within +0.5 ms of lock60gs-v. Any fault or a counter not 0: every other simt run is void." + SIMT_NEEDS),
    _r17("simt-v", "Overlapped step (MSC_SIMT=2, MSC_RENDER_RNG=2), LOCKED 60 Vice", "tour", _m(GS60, SIMT),
         "overlap18: game-thread p95 with / without a step, `[simt] overlap:` window and join wait, late % against lock60gs-v, the `[fs] np_sim` line. Rules: a single `JOIN TIMEOUT` right after a PS-button suspend is noise, any other is a fail; MORE late pictures means a suspected heap-lock sleep, not 'no gain'. Ship rule (the held-30 and per-class table, later): p95 at least 2 ms under lock60gs-v, late % not higher, no audio underrun, counters clean." + SIMT_NEEDS),
    _r17("simt-d", "Overlapped step (MSC_SIMT=2, MSC_RENDER_RNG=2), LOCKED 60 The Dump", "worstgpu", _m(GS60, SIMT), "overlap18: as simt-v at The Dump against lock60gs-d." + SIMT_NEEDS),
    _r17("simt-lava", "Overlapped step, LOCKED 60 Lava Pit", "clava", _m({"MSC_LOCK60": "1", "MSC_FX_STAT": "1", "MSC_HEAD_STAT": "1"}, SIMT), "overlap18: Lava Pit against lock60lava." + SIMT_NEEDS),
    _r17("simt30-d", "Overlapped step at the held 30 (no LOCK60), The Dump goal scenario", "gsdump", _m({"MSC_GSPLIT": "2"}, SIMT),
         "overlap18: held 30 at The Dump: late %, goal-cut spikes, speed line 98 %, against gsdump16. Turn it on for the held 30 when this lowers late pictures." + SIMT_NEEDS),
    _r17("simt-c1", "Overlapped step, np_sim on core 1 (MSC_SIMT_CORE=1)", "tour", _m(GS60, SIMT, {"MSC_SIMT_CORE": "1"}), "overlap18: where the sim thread belongs on a stock console (default under 2 is core 2); against simt-v." + SIMT_NEEDS),
    _r17("simt-c12", "Overlapped step, np_sim on cores 1+2 (MSC_SIMT_CORE=12)", "tour", _m(GS60, SIMT, {"MSC_SIMT_CORE": "12"}), "overlap18: as simt-c1, either core." + SIMT_NEEDS),
    _r17("simt-fs12", "Overlapped step, frame send on cores 1+2 (MSC_VITA_CORE_FRAMESEND=12)", "tour", _m(GS60, SIMT, {"MSC_VITA_CORE_FRAMESEND": "12"}), "overlap18: frame send floating off the sim core; against simt-v." + SIMT_NEEDS),
    _r17("simt-c3", "Overlapped step with the fourth core (MSC_VITA_CORE3=1, MSC_VITA_CORE3_SEND=1)", "tour", _m(GS60, SIMT, {"MSC_VITA_CORE3": "1", "MSC_VITA_CORE3_SEND": "1"}),
         "overlap18: ONLY if the core-3 probe line (vita_probe.cpp) of an earlier log says 'usable' (CapUnlocker plug-in present); if it says 'NOT usable' the run is SKIPPED, not failed." + SIMT_NEEDS),
    _r17("simt-lat", "Overlapped step with the sampled-late pad (MSC_LAT_SAMPLE=1)", "tour", _m(GS60, SIMT, {"MSC_LAT_SAMPLE": "1"}), "overlap18: press-to-picture ([lat] p50 / p95) unchanged against simt-v and lock60gs-v." + SIMT_NEEDS),
    # ---- block B: the PGO runs at 60 ----
    _r17("pgo-base-lock60", "PGO A/B at 60: the plain executable, LOCKED 60 + head lines, Vice", "tour", GS60, "plan-wave17: the same env as lock60gs-v on the plain executable (a repeat of it, for the drift); the pair of pgo-on-lock60."),
    _r17("pgo-on-lock60", "PGO A/B at 60: the PGO executable, LOCKED 60 + head lines, Vice", "tour", GS60, "plan-wave17: only the eboot differs from pgo-base-lock60 (`[head]` game-thread p95 is what the PGO build must cut). Watch the first 60 s for a crash."),
    _r17("simt-pgo", "Overlapped step on the PGO executable, LOCKED 60 Vice", "tour", _m(GS60, SIMT), "overlap18: the executable that would ship, against pgo-on-lock60 (the same env without the SIMT keys)." + SIMT_NEEDS),
    # ---- block C0: the section-ordered PGO executable (link17; needs the r17pgox / r17pgoxm executables) ----
    _r17("pgox-d1-a", "Section-ordered PGO executable, Vice (a)", "tour", {}, "link17: the same env as d1; only the eboot differs (s17-pgo-x: hot functions ordered from the pgo16 profile). Against pgo-on-d1-a/b."),
    _r17("pgox-d1-b", "Section-ordered PGO executable, Vice (b)", "tour", {}, "link17: as pgox-d1-a."),
    _r17("pgox-wg-a", "Section-ordered PGO executable, The Dump (a)", "worstgpu", {}, "link17: as pgox-d1-a, against pgo-on-wg-a/b."),
    _r17("pgox-wg-b", "Section-ordered PGO executable, The Dump (b)", "worstgpu", {}, "link17: as pgox-wg-a."),
    _r17("pgox-lock60", "Section-ordered PGO executable, LOCKED 60 + head lines, Vice", "tour", GS60, "link17: against pgo-on-lock60: `[head]` game-thread p95."),
    _r17("pgoxm-d1", "Section-ordered PGO executable with the SDK maths in the LTO unit (s17-pgo-xm), Vice", "tour", {}, "link17: (b) NP_MTX_LTO=1 on top of the order. Only if pgox-d1-a/b gained: ship order-only unless this gains more."),
    # ---- block C: GPU levers for The Dump / Lava at 60, memory sites, cheap probes ----
    _r17("lock60x-dump", "LOCKED 60 with the GPU levers on (DLWELD + HSR_PROVEN/FACTS + RENDER_NEON), The Dump", "worstgpu", _m({"MSC_LOCK60": "1", "MSC_HEAD_STAT": "1", "MSC_FX_STAT": "1", "MSC_VGX_HSR_PROVEN": "1", "MSC_VGX_HSR_FACTS": "1", "MSC_VGX_HSR_COUNT": "1", "MSC_RENDER_NEON": "1"}),
         "plan-wave17: only meaningful once exactall-* verified at 0: GPU p95 and relief against lock60dump / lock60gs-d."),
    _r17("lock60x-lava", "LOCKED 60 with the GPU levers on, Lava Pit", "clava", _m({"MSC_LOCK60": "1", "MSC_HEAD_STAT": "1", "MSC_FX_STAT": "1", "MSC_VGX_HSR_PROVEN": "1", "MSC_VGX_HSR_FACTS": "1", "MSC_VGX_HSR_COUNT": "1", "MSC_RENDER_NEON": "1"}),
         "plan-wave17: as lock60x-dump at Lava Pit against lock60lava."),
    _r17("memsite-d", "Video memory by allocation site (MSC_VGL_MEMSITES=1), Dump goal scenario", "gsdump", {"MSC_GSPLIT": "2", "MSC_VGL_MEMSITES": "1"},
         "memsite17: `[memsite]` tables (KiB and blocks per site per pool) at kick-off, at the first held cut and at the first FAILED (at most 6 dumps); run tools/console-1.2/memsite_resolve.py on the log. Against gsdump16: same counters. Diagnostic, answers who holds the 24 MiB."),
    _r17("memsite-v", "Video memory by allocation site (MSC_VGL_MEMSITES=1), Vice goal scenario", "gsvice", {"MSC_GSPLIT": "2", "MSC_VGL_MEMSITES": "1"}, "memsite17: as memsite-d at Vice, against gsvice16."),
    _r17("price17ref", "Game-thread reference with the split lines (tour)", "tour", {"MSC_GSPLIT": "2"}, "price17 / probe18: the base of the four runs below (they read `[gsplit]` begin-frame and sim ms, so the base needs the split lines on)."),
    _r17("price17a", "AI script clock read as the game had it (MSC_AI_NOCLOCK=0; plan default 1 since r17), split lines on", "tour", {"MSC_GSPLIT": "2", "MSC_AI_NOCLOCK": "0"}, "price17: game-thread ms with / without the AI clock (80 reads a picture): `[gsplit]` begin-frame and sim against price17ref. Exact on the host (det equal, BREAK caught)."),
    _r17("cpu22v-v", "cpu22 exact cuts both ways at Vice (MSC_CPU22_*=2)", "tour", {"MSC_CPU22_INVLAZY": "2", "MSC_CPU22_POWERUP": "2", "MSC_CPU22_REPLAY": "2"}, "cpu22 verify: every `[cpu22] <name> mismatches N` line must read 0 (INVLAZY, POWERUP, REPLAY); any other number = the switch stays off. Mode 2 also runs the old way, so ms are not judged here."),
    _r17("cpu22v-d", "cpu22 exact cuts both ways at The Dump (MSC_CPU22_*=2)", "worstgpu", {"MSC_CPU22_INVLAZY": "2", "MSC_CPU22_POWERUP": "2", "MSC_CPU22_REPLAY": "2"}, "cpu22 verify at The Dump: every `[cpu22] <name> mismatches N` line must read 0; any other number = the switch stays off."),
    _r17("cpu22on-v", "cpu22 exact cuts on at Vice (MSC_CPU22_*=1), split lines on", "tour", {"MSC_GSPLIT": "2", "MSC_CPU22_INVLAZY": "1", "MSC_CPU22_POWERUP": "1", "MSC_CPU22_REPLAY": "1"}, "cpu22: game-thread ms from [gsplit] against price17ref (estimate 0.2-0.3 ms); only after cpu22v-v and cpu22v-d read 0 mismatches. Then the coordinator puts the three keys at 1 in the plan."),
    _r17("price17b", "FPS probe off (MSC_FPS=30), split lines on", "tour", {"MSC_GSPLIT": "2", "MSC_FPS": "30"}, "price17: begin-frame pace with the probe off (it reads only at frame 1 in 30 mode); against price17ref. Not a candidate, a measurement of what the probe costs."),
    _r17("probe18a", "FPS probe every 4th picture (MSC_FPS_PROBE_EVERY=4), split lines on", "tour", {"MSC_GSPLIT": "2", "MSC_FPS_PROBE_EVERY": "4"}, "probe18: bf-pace ms against price17ref, the `[fps]` line 'read in N pictures', switches and decisions against the base, [hitch] count. Changes the controller's input (a stale reading moves a decision by up to N pictures): needs the decisions equal."),
    _r17("probe18b", "FPS probe every 4th picture and AI clock off, split lines on", "tour", {"MSC_GSPLIT": "2", "MSC_FPS_PROBE_EVERY": "4", "MSC_AI_NOCLOCK": "0"}, "probe18: the probe every 4th picture with the AI clock read as before (the plan has MSC_AI_NOCLOCK=1 since r17): against price17a."),
    _r17("freezeidle", "Freeze button armed, nobody presses it (MSC_FREEZE_BUTTON=1), The Dump", "worstgpu", {"MSC_FREEZE_BUTTON": "1"}, "galaxyport18: the whole Dump route with the button thread running: no FATAL line (the thread costs nothing visible); frame times against worstgpu1."),
    _r17("freezebutton", "Freeze button pressed by hand (MSC_FREEZE_BUTTON=1), The Dump  -- BY HAND", "worstgpu", {"MSC_FREEZE_BUTTON": "1"},
         "galaxyport18: HANDS: a person holds L+R+Select for 4 s, 30+ s into the Dump walk-out: FATAL + thread lines in log.txt, and psp2core-*.psp2dmp in ux0:data (is the dump written with kubridge loaded?). In no unattended list."),
    # ---- the settings screens that ship (live19, scene18): driven by the route's own pad and finger presses ----
    _r17("live19-shake", "LESS SHAKE and CROWD applied at once from the in-match settings, saved", "live19", {},
         "live19: the route (fb-live19: CROWD -> HIDE, LESS SHAKE -> ON, leave popup, SAVE). Log: `[live] MSC_PERFORMANCE_MODE=1`, `[live] MSC_REDUCED_MOTION=1`, STATE lines to `back on the pause menu`, no restart (one process, no second boot line). By eye (the owner): the shake is a quarter at once on a hit or a Mega Strike. Read [hitch] right after the save: no hitch."),
    _r17("live19-crowd", "CROWD HIDE then SHOW applied at once", "live19c", {}, "live19: the stands empty at the save and refill at the second save, no hitch (`[hitch]` lines after each `STATE saved`), `[live]` lines for each."),
    _r17("live19-rear", "REAR TOUCH OFF then ON applied at once", "live19r", {}, "live19: two `[live]` lines for the rear switch, no restart; BY HAND the back panel stops and starts (the route cannot press the back panel)."),
    _r17("live19-leave", "Leave dialog on a next-start row (SMOOTH 60, Circle): SAVE / DON'T SAVE / KEEP EDITING", "live19l", {}, "live19: the dialog shows the next-start note; SAVE does not restart (STATE `back on the pause menu` in the same process). By eye: the text fits on the OLED."),
    _r17("scene18-pause", "Pause menu -> VITA SETTINGS screen driven by the route (pad and finger), pages and popup", "pause", {}, "scene18 + menufix17: fb-pause (emulator-proven). Log: STATE lines to the end, 0 faults. The real finger and the OLED readability are BY HAND (CONSOLE-PLAN-r17 hands list)."),
    # ---- heat shimmer (warble19): the defaults are ON in the plan; these pin each side so the log proves it ----
    _r17("warboff-l", "Heat shimmer cuts all off, LOCKED 60 Lava Pit", "clava", _m({"MSC_LOCK60": "1", "MSC_FX_STAT": "1", "MSC_HEAD_STAT": "1", "MSC_VGX_CLEARVEH": "0", "MSC_VGX_EFBSWAP_NEXT": "0", "MSC_VGX_PASSISO_COVER": "0", "MSC_WARBLE_LITE": "0"}), "warble19: the OFF side at Lava Pit at 60: [lane] gpu and late % against warb-l and warbl-l."),
    _r17("warb-l", "Heat shimmer: the three exact cuts on, map every picture (LITE 0), LOCKED 60 Lava Pit", "clava", _m({"MSC_LOCK60": "1", "MSC_FX_STAT": "1", "MSC_HEAD_STAT": "1", "MSC_WARBLE_LITE": "0"}), "warble19: the three exact cuts are the plan default (so no key is written: the kit refuses a run that writes a default); LITE 0 = 2 scenes fewer (exact: 0 pixels differ on the host). Read [lock60], [lane] gpu, late %."),
    _r17("warboff-s", "Heat shimmer cuts all off, Sand Tomb", "sand", {"MSC_VGX_CLEARVEH": "0", "MSC_VGX_EFBSWAP_NEXT": "0", "MSC_VGX_PASSISO_COVER": "0", "MSC_WARBLE_LITE": "0"}, "warble19: the OFF side at Sand Tomb (fb-sand): [lane] gpu and late % against warb-s and warbl-s."),
    _r17("warb-s", "Heat shimmer: the three exact cuts on (LITE 0), Sand Tomb", "sand", {"MSC_WARBLE_LITE": "0"}, "warble19: the exact cuts (plan default, no key) with LITE 0 = 2 scenes fewer at Sand Tomb."),
    _r17("warbl-s", "Heat shimmer: cuts + half-rate half-size map (MSC_WARBLE_LITE=3), LOCKED 60 Sand Tomb", "sand", _m({"MSC_LOCK60": "1", "MSC_FX_STAT": "1", "MSC_HEAD_STAT": "1", "MSC_WARBLE_LITE": "3"}), "warble19: bit 2 adds the half-size map (also acts at 30; bit 1 is the 60 mode)."),
    # ---- census19: tight particle polygons with the compositor off on both sides ----
    _r17("fxc0-pipe", "Effects compositor off (the control), Pipeline Central", "cpipe", {"MSC_FX_COMPOSITE": "0"}, "census19: the control side for fxtight-c0-pipe (tight polygons only match the picture with the compositor off)."),
    _r17("fxtight-c0-pipe", "Tight particle polygons with the compositor off, Pipeline Central", "cpipe", {"MSC_FX_COMPOSITE": "0", "MSC_FX_TIGHT": "8", "MSC_FX_TIGHT_DROP": "1", "MSC_FX_TIGHT_STAT": "300"}, "census19: invisible with the compositor off (host: max 2/255), saves 0.1-1.0 screens in cut-scenes, 0.03-0.2 in play. Against fxc0-pipe: GPU ms; the [fxtight] lines."),
    _r17("fxc0-lava", "Effects compositor off (the control), Lava Pit", "clava", {"MSC_FX_COMPOSITE": "0"}, "census19: the control side for fxtight-c0-lava."),
    _r17("fxtight-c0-lava", "Tight particle polygons with the compositor off, Lava Pit", "clava", {"MSC_FX_COMPOSITE": "0", "MSC_FX_TIGHT": "8", "MSC_FX_TIGHT_DROP": "1", "MSC_FX_TIGHT_STAT": "300"}, "census19: as fxtight-c0-pipe at Lava Pit."),
    # ---- the shader pack candidate (list17): a package with the pack19 list, installed by `swap pack19` ----
    _r17("l17pack19", "Shader pack19 (489 programs) with the warm-up cap 500 (MSC_WARM_CAP=500), Vice", "tour", {"MSC_WARM_CAP": "500"},
         "list17: pack19 holds 489 programs / 769 variants (pack16 329 / 565); the cap 400 would leave 89 to first use. Against d1: `[boot]` present 2 time (the cost: console boot cost unmeasured) and `[cuts]` first uses at the first goal, replay and walk-out (the gain). One fragment shader (B8A601C15245AAA5, 3 programs) still compiles on the console. Needs versions/pack19 (`swap pack19`, then `swap 1.3` to put the r17 package's own pack back).", version="pack19", tier=3, role="manual"),
]

# dumpgpu20 section 3 (Fable): the Dump's extra GPU time. Every run pins the size (MSC_PICTURE_HOLD_FORCE=10: 704x400, one sample) unless it names one; read `[lane] gpu` in open play, three windows or more.
_F10 = {"MSC_PICTURE_HOLD_FORCE": "10"}
RUNS += [
] + [
    _r17("dg20-s2-size-d%d" % w, "Dump GPU S2: one-sample size %dx%d, The Dump" % (w, h), "worstgpu", {"MSC_VITA_RENDER": "%dx%d" % (w, h), "MSC_VITA_RT_TIMERS": "2", "MSC_VITA_RT_LOG": "300"},
         "dumpgpu20 S2: one-sample slope and intercept in one build (a fixed size turns the GPU timers off; the two timer keys put them back: check `[lane] gpu` is not 0).")
    for w, h in ((960, 544), (912, 520), (864, 488))
] + [
    _r17("dg20-s3-size-v%d" % w, "Dump GPU S3: one-sample size %dx%d, Vice" % (w, h), "tour", {"MSC_VITA_RENDER": "%dx%d" % (w, h), "MSC_VITA_RT_TIMERS": "2", "MSC_VITA_RT_LOG": "300"},
         "dumpgpu20 S3: Vice's slope: decides fixed work against per-pixel excess at The Dump.")
    for w, h in ((960, 544), (912, 520), (864, 488))
] + [
    _r17("dg20-s%d-%s" % (n, tag), "Dump GPU S%d: %s, The Dump" % (n, what), "worstgpu", _m(_F10, e), "dumpgpu20 S%d: %s" % (n, why))
    for n, tag, what, e, why in (
        (4, "flat-d", "flat shading hunt", {"MSC_VGX_HUNT": "flat"}, "base minus flat = shading + texture cost (H1). Must print `flat` acting, not DID NOTHING."),
        (5, "pixel-d", "no-fill hunt", {"MSC_VGX_HUNT": "pixel"}, "what is left with no fill: the true fixed part (H3)."),
        (6, "blend-d", "blended draws off", {"MSC_VGX_HUNT": "blend"}, "all blended draws gone (rain, shadow quad, effects): H2 in ms; a gain of 2 ms or more starts shadowst19."),
        (7, "aniso0-d", "anisotropic filter off", {"MSC_VITA_ANISO": "0"}, "filter share of H1."),
        (8, "lod0-d", "mip bias rung 0", {"MSC_VGX_LODBIAS_RUNG": "0"}, "mip-bias share of H1."),
        (9, "pb8-d", "parameter buffer 8 MiB", {"MSC_VITA_PB_MIB": "8", "MSC_VGX_NODEPTH_PASS": "0"}, "unchanged p50 AND p95 = no partial renders at 16 MiB (and 8 MiB of video memory to win)."),
        (10, "pb32-d", "parameter buffer 32 MiB", {"MSC_VITA_PB_MIB": "32"}, "ONLY if S9 is worse. Takes 16 MiB from a pool that peaks at 94196 of 94208 KiB: may fail to load."),
        (11, "passes-d", "off-screen passes hunt", {"MSC_VGX_HUNT": "passes"}, "atlas and other off-screen work in ms."),
        (12, "rain2-d", "rain one drop in two", {"MSC_STADIUM_CUT": "64"}, "`[stadiumcut]` count over 0. Expect under 0.5 ms."),
        (13, "skin0-d", "GPU skinning off", {"MSC_SKIN_GPU": "0"}, "expect no change in play (H ranked out)."),
        (14, "up-d", "final picture pass skipped", {"MSC_VGX_HUNT": "upskip"}, "the final picture pass, today assumed 2 ms."))
]

# r17 second pass (kit20): vram19 (patcher buffer, parameter buffer in LPDDR, display depth), gpuscene20 diagnostics, rain-off hunt
RUNS += [
    _r17("patcherbuf", "Shader patcher buffer 2 MiB (MSC_VGL_PATCHER_BUF_KB=2048), Vice", "tour", {"MSC_VGL_PATCHER_BUF_KB": "2048"},
         "vram19: does 2 MiB of patcher buffer (default 8192) give 0 `patching failed` and the warm gate (329 / 489 programs linked before the match)? Read the `[vgx-vita] patcher buffer high-water` line. Pass: 0 patching failed, the gate as in d1, the high-water under 2048 KiB."),
    _r17("pbl1", "Parameter buffer in LPDDR (MSC_VITA_PB_LPDDR=1), Vice", "tour", {"MSC_VITA_PB_LPDDR": "1"},
         "vram19: does sceGxm take the flag in a normal app (`PB_LPDDR: TAKEN` or REFUSED), does `pools requested ... cdram` rise by about 16 MiB, are the GPU ms the same as d1. A HANG or a fault = fail (the switch stays off)."),
    _r17("dispdepth", "Display depth without memory (MSC_VGX_DISPDEPTH=nomem), Vice  -- BY HAND", "tour", {"MSC_VGX_DISPDEPTH": "nomem"},
         "vram19: HANDS: play a match, open the keyboard (online name entry) and the ad hoc NetCheck dialog: are the dialogs drawn? `display depth:` made / freed counters. In no unattended list."),
    _r17("swz-d", "Swizzled textures (MSC_VGX_TEXSWIZZLE=1) with the class census, The Dump", "worstgpu", {"MSC_VGX_TEXSWIZZLE": "1", "MSC_VGX_TEXCLASS": "1", "MSC_HEAD_STAT": "1"},
         "swz20: `[lane] gpu` at The Dump against worstgpu1 (lower is the gain), EVERY HUD icon drawn right (any wrong icon = stop the swz runs), the `[texclass]` counts. Needs `[texclass]` lines (INVALID if missing). MSC_VGX_TEXSWIZZLE=1 goes into the plan only after a clean console round shows lower GPU ms at The Dump and no wrong icon.", tier=3),
    _r17("swz-v", "Swizzled textures with the class census, Vice tour", "tour", {"MSC_VGX_TEXSWIZZLE": "1", "MSC_VGX_TEXCLASS": "1", "MSC_HEAD_STAT": "1"},
         "swz20: as swz-d at Vice against d1 (same pass rule: any wrong icon = stop the swz runs).", tier=3),
    _r17("swz-m", "Swizzled textures with mip chains (MSC_VGX_TEXSWIZZLE_MIPS=1), The Dump", "worstgpu", {"MSC_VGX_TEXSWIZZLE": "1", "MSC_VGX_TEXSWIZZLE_MIPS": "1", "MSC_VGX_TEXCLASS": "1", "MSC_HEAD_STAT": "1"},
         "swz20: whether mip chains are safe (the original wrong-icon case). Only if swz-d was clean (every icon right); a wrong icon here only closes the MIPS switch.", tier=3),
    _r17("scenes-d", "Dump GPU diagnostics: per-scene GPU ms, parameter-buffer estimate, texture layout (all four on), The Dump", "worstgpu", _m(_F10, {"MSC_VITA_RT_SCENES": "1", "MSC_VGX_PBEST": "1", "MSC_VGX_TEXCENSUS20": "1"}),
         "gpuscene20 (replaces dumpgpu20 S1): per-scene GPU ms (`MSC_VITA_RT_SCENES`), `[pb est]` parameter-buffer estimate, texture layout census. The emulator showed 47 guest faults with all the gpuscene20 diagnostics on (none with them off): a fault HERE is a diagnostic failure, not a game failure. Run LAST in its unit."),
    _r17("scenes-v", "Dump GPU diagnostics, Vice (the figures to subtract)", "tour", _m(_F10, {"MSC_VITA_RT_SCENES": "1", "MSC_VGX_PBEST": "1", "MSC_VGX_TEXCENSUS20": "1"}), "gpuscene20: as scenes-d at Vice (the same fault caveat)."),
    _r17("rainoff-d", "Rain-off hunt (MSC_VGX_HUNT=rain), The Dump", "worstgpu", _m(_F10, {"MSC_VGX_HUNT": "rain"}), "gpuscene20: the true rain-off GPU ms; needs `[hunt] rain` above 0 (DID NOTHING = the run is INVALID). After S6."),
]

# ---- online24: the unit ON of the extra list: the REAL online service (msc-online, strikers.psvita.dev and the old address) exercised by ONE console through the game's own menus ----
ON_PASS = (" PASS (tools/console-1.2/online_check.py <log> --run <id> reads it all): every STATE line of the route in order and `script exit`, no `[script] TIMEOUT`, no crash; the `[np-net] req` lines show every request ANSWERED by the "
           "service (an HTTP 2xx or the service's own JSON error with a code), no `trying the other address` line (either `[np-net]` or `[online]`: in an auto run one means the new address FAILED: report it), host=new on every request "
           "of an auto run, host=old in the old variant; at most 100 requests in any 10 s window (the zone's shared rule) and the requests per minute stay far below the service's daily budget; every pollScale / over_budget level / "
           "retryAfterSec the log shows is reported; the menu returns after the cancel. Needs a package built from agent/online24 (MSC_NET_REQLOG: before it the client logs no request line and the rules cannot be read).")
ON_WHY = {
    "on-hub": "(a) the online menu opens and the profile is present: MULTIPLAYER -> ONLINE -> RANKED MATCH -> Online Central (login: `[online] login ok: profile N` with `(new account)` the first time ever, then the hub). The public name is MSC_ONLINE_NAME (nothing typed; the system keyboard never opens). Address rule: auto (new first, fallback to the old); the stock build's default is old.",
    "on-hub-old": "(a, old variant) the same route with MSC_ONLINE_HOST=old: every request host=old, the answers as on-hub; the pair says whether the new address answers what the old one does.",
    "on-conn": "(b) the connection check (mpdiag18, np_conntest) -- BY HAND, in no unattended list: its only entry is the failure popup's TEST CONNECTION after HOST A ROOM failed to reach the service, and the isolated build's env reader refuses an MSC_ONLINE_URL (a `/` or `:` in a value), so nothing in np-player.txt can make the first try fail. HANDS: put the Vita on a Wi-Fi that has no internet (router WAN unplugged, or a hotspot with data off), then `bash session.sh run on-conn`. The route presses HOST A ROOM, waits for the failure popup (2 buttons), presses TEST CONNECTION and reads the result popup. Answers: the verdict sentence (`[online-menu]` lines, popup by eye) and `[np-net] req ... -> FAIL` lines. Pass differs from the others: FAIL lines are expected here, nothing else; the healthy-service result of the check has no menu entry.",
    "on-quick": "(c) quick match: PLAY WITH ANYONE (MSC_ONLINE_FRIENDS=1 offers it: the tree does not yet) joins the unranked queue (`[online-menu] chose anyone (unranked quick match)`; on the console the DWC line `quick match: unranked` follows), 60 s wait with nobody to play, Circle cancels (`[ranked] matchmaking cancelled`), back to the main menu. The queue polls about every 2 s: count the POST /v1/ranked/queue lines.",
    "on-ranked": "(d) ranked queue (the hub's RANKED MATCH, the same service call without mode quick): 60 s wait, cancel, back to the main menu.",
    "on-lists": "(e) read-only views: the hub's FRIENDS (friends list) and RANKINGS (leaderboard) screens; nothing is written (the login's own rank put is the only POST besides the reads).",
    "mp-hub": "mp18 (owner 23 Nov 'make sure multiplayer works'; ONE Vita): the on-hub route with TODAY's plan (fourth core on, LOCKED 60 hold, GT18, held governor). Besides the online rules: `[vita] core 3 rules: an online or LAN session is up: N thread(s) moved off the system core` when the online screens open (the online guard, np_vita_core3.h). Needs CapUnlocker (without it `fourth core: not available` and the guard line cannot appear: report, not a failure of multiplayer).",
    "mp-quick": "mp18: the quick-match queue (on-quick's route) with today's plan: 60 s in the queue with nobody, cancel; the guard line as mp-hub. The region and name are the account's (MSC_ONLINE_NAME; the region is given at the account's first sign-up only).",
    "mp-host": "mp18: HOST A ROOM alone: `[np-net] UDP port 25110 bound (asked 25110, ...)` (the port17 bind chain: never 41000), STUN, POST /v1/rooms, 45 s in the waiting popup (stage 3), Circle: `[online-menu] cancelled while connecting (stage 3)`; the guard moves the core-3 threads off and, 3 s after the menu, back (`no online session any more`).",
    "mp-host-relay": "mp18: as mp-host with RELAY ONLY (MSC_NET_RELAY_ONLY=1, the Settings choice): the room asks the service for TURN credentials and allocates a relay before it publishes anything: `relay: allocating on <server>` then `relay ready: <address>` = the TURN relay is reachable from this Wi-Fi (the fallback every online match through a strict NAT needs). A `relay failed` / failure popup instead = report the line (the Wi-Fi blocks the relay's UDP, or the service's TURN key / budget is off). Uses one of the account's 6 TURN credentials a day.",
    "mp-lan-wifi": "mp18: Same Wi-Fi host alone: LOCAL -> SAME WI-FI, Create Game, 60 s in the lobby with nobody, out. `[vlink-vita] UDP port 25100 bound (asked 25100, 1 bind tried)` (never 41000), the guard's off / back lines, no request to the online service. A second device on the same Wi-Fi that sees the game is the partner test (mp18.md).",
    "mp-lan-adhoc": "mp18: NEARBY (the Vita's ad hoc radio) host alone: the radio comes up (the system's Wi-Fi dialog may show: OK it by hand if it does), Create Game, 60 s, out; the guard's off / back lines. Needs no access point.",
    "on-report": "(f) ONLINE page > SAVE A REPORT on the pixel screen (opened by the np-settings-open hook; Vita-only code, no host proof): log `[settings] save a report: written`; then fetch ux0:data/mscharged-perf-test/mp-report.txt by hand (no names, addresses or codes in it). A fresh process: the report has the build and settings rows and no online events (the screen is not reachable after the online menu in one process).",
}
RUNS += [
    _r17(sp["id"], sp["title"], sp["route"], dict(_GO.COMMON_ENV, **dict(sp["env"])), ON_WHY[sp["id"]] + ("" if sp["id"] in ("on-conn", "on-report") else ON_PASS), tier=3, group="mp18" if sp["id"].startswith("mp-") else "online24",
         **({"role": "settings", "settings": _GO.SETTINGS_REPORT} if sp["id"] == "on-report" else {"role": "manual"} if sp["id"] == "on-conn" else {}))
    for sp in _GO.RUN_SPECS
]

# be24 (wave 12, back end CPU): the render thread's CPU is most likely the CPU's writes into UNCACHED RAM (about 1.4 MB a picture, 0.25 GB/s measured on 5 Oct = 5.5 ms): these runs measure it and the three cuts
BE_BASE = {"MSC_GSPLIT": "2", "MSC_VGX_BEPH": "1", "MSC_VGX_GXM_DIRECT": "1"}
BE_WHY = " Read `[beph]` (render-thread CPU per picture by preparation step, command kind and draw phase; stream bytes by draw class) and `[lane]` be CPU, Vice goal scenario, live play pictures. Pass rule: no new [hitch], no fault, the picture as in the base run."
RUNS += [
    _r17("bephbase", "be24 base: back end phase profile, Vice goal scenario", "gsvice", BE_BASE, "The baseline for the three be24 cuts: where the 5.8-8.5 ms of live-play back end CPU go." + BE_WHY, group="be24"),
    _r17("wrcopybench", "be24 WRCOPY: start-up benchmark only (MSC_VITA_WRCOPY=bench)", "gsvice", dict(BE_BASE, MSC_VITA_WRCOPY="bench"),
         "Read the `[wrcopy]` line: MB/s of plain, NEON 64, vstm 128 and DMA copies into an uncached block, each verified. A MISMATCH disqualifies the variant (then no WRCOPY run follows). Changes nothing else; the run answers whether the uncached write rate is the bottleneck (a variant over 1.3x plain says yes)." + BE_WHY, group="be24"),
    _r17("wrcopy0", "be24 WRCOPY off (plain copies; the plan default is auto since wave 24)", "gsvice", dict(BE_BASE, MSC_VITA_WRCOPY="0"),
         "Against bephbase: `[beph]` cpu ms/pic and the issue / upload commands should fall by (stream+ring MB x (3.9 ms/MB - 1/rate)); the picture is byte for byte the same (the copies are the same bytes). Ship if cpu falls 1 ms or more." + BE_WHY, group="be24"),
    _r17("wrcopydma", "be24 WRCOPY forced to the DMA variant (MSC_VITA_WRCOPY=3)", "gsvice", dict(BE_BASE, MSC_VITA_WRCOPY="3"),
         "Only if wrcopybench shows dma verified. Against wrcopyauto: sceDmacMemcpy is synchronous in the SDK; a corrupt picture or a hang rejects it." + BE_WHY, group="be24"),
    _r17("dyncache1", "be24 DYNCACHE: player vertex ranges not rewritten (MSC_VGX_DYNCACHE=1)", "gsvice", dict(BE_BASE, MSC_VGX_DYNCACHE="1"),
         "Read `[dyncache]`: hits per picture, KiB left out of the stream (about 500 expected), refused 0; `[gxm-direct]` refused 0. The six players must look as in bephbase (a stale or wrong player = reject). Against wrcopyauto: cpu ms/pic falls by the KiB saved x the measured ms/MB." + BE_WHY, group="be24"),
    _r17("dyncache2", "be24 DYNCACHE with the byte compare (MSC_VGX_DYNCACHE=2), short check", "gsvice", dict(BE_BASE, MSC_VGX_DYNCACHE="2"),
         "`[dyncache]` mismatch must be 0 over the live-play pictures (doubles the cached memory; a short check, not a timing run)." + BE_WHY, group="be24"),
    _r17("streamcdram-be", "be24: the stream arena in video memory (MSC_VITA_VGL_STREAM_KIB=2048, never run on the s27 line)", "gsvice", dict(BE_BASE, MSC_VITA_VGL_STREAM_KIB="2048", MSC_VITA_VGL_STREAM_MEM="cdram"),
         "Against bephbase: the same stream written to a memory the CPU writes faster? CDRAM is full at kick-off (it may refuse: the log line says so)." + BE_WHY, group="be24"),
    _r17("streamcached-be", "be24: the stream arena in cached memory (MSC_VITA_VGL_STREAM_KIB=2048, MEM=cached)", "gsvice", dict(BE_BASE, MSC_VITA_VGL_STREAM_KIB="2048", MSC_VITA_VGL_STREAM_MEM="cached"),
         "As streamcdram-be; needs a write-back per picture (the log line says whether it worked). A stale picture rejects it." + BE_WHY, group="be24"),
]

# be25 (wave 12): the first console run of the profiler (bephbase, 17 Nov) showed that its "cpu" was WALL time (49 ms in the goal cut-scene against 21-24 ms of thread CPU in [lane]); v2 prints wall / cpu per phase.
# fastcmp (MSC_VGX_FASTCMP, Vita plan ON) and the prefetch are the two cuts for the uniforms / pre phases of Draw (18 + 7 us a draw measured).
BE25_WHY = " Read `[beph]` (v2: every figure wall/cpu; scene 3(cut) is the goal cut-scene and walk-out, `uniforms` and `pre` are us per draw), `[lane]` be CPU / wall and `[fastcmp]`. Pass rule: no new [hitch], no fault, the picture as in the base run."
RUNS += [
    _r17("bephv2", "be25 base: wall / cpu profile of the back end, Vice goal scenario (FASTCMP on as planned)", "gsvice", BE_BASE,
         "The v2 profile: which phases of the cut-scene picture are CPU (thread clock) and which wait (display swap, GPU queue, scene ends). Expect cut-scene cpu near the [lane] be CPU (21-24 ms), wall near 49. Answers where the 7.8 ms `ct` and the 16 ms `draw` of the v1 run were CPU or waiting." + BE25_WHY, group="be25"),
    _r17("fastcmp0", "be25 A/B: the uniform block compare by memcmp (MSC_VGX_FASTCMP=0)", "gsvice", dict(BE_BASE, MSC_VGX_FASTCMP="0"),
         "Against bephv2 (FASTCMP=1, the default): `uniforms` us per draw (wall and cpu) should be higher here by the memcmp time (estimate 5-10 us a draw in the cut-scene), cpu ms/pic higher by 1.5-3 ms; the picture is identical. `[fastcmp]` shows block compares and KiB read per picture (about 0.5 MB expected)." + BE25_WHY, group="be25"),
    _r17("bepf2", "be25: prefetch the next draws' records and uniform blocks (MSC_VGX_BEPREFETCH=2)", "gsvice", dict(BE_BASE, MSC_VGX_BEPREFETCH="2"),
         "Against bephv2: `pre` and `uniforms` us per draw (cpu) and cpu ms/pic should fall if those phases wait for memory; `[fastcmp]` draws prefetched about the draws of the picture. Ship (plan ON) if cpu falls 0.5 ms or more with no change anywhere else." + BE25_WHY, group="be25"),
    _r17("bepf4", "be25: prefetch four draws ahead (MSC_VGX_BEPREFETCH=4)", "gsvice", dict(BE_BASE, MSC_VGX_BEPREFETCH="4"),
         "As bepf2, a longer distance; run only if bepf2 helped. Pick the distance that lowers cpu ms/pic most." + BE25_WHY, group="be25"),
]

# be26 (wave 12): the render thread's cost per draw. MSC_VGX_UNICMP (uniform block compared with vitaGL's own buffer, no shadow copy) and MSC_VGX_VGL_UNIHIST (a program's uniform buffer that equals
# one of its last four copies of the scene names that ring copy again) are both plan ON; the off runs are the A/B. BEPH_EVERY=1 so the profiler prints on these short routes.
BE26_BASE = {"MSC_GSPLIT": "2", "MSC_VGX_BEPH": "1", "MSC_VGX_BEPH_EVERY": "1", "MSC_VGX_GXM_DIRECT": "1"}
BE26_WHY = " Read `[beph]` us per draw (pre / uniforms / issue) and cpu ms/pic for scene 3(cut), `[lane]` be cpu, `[vgx] vitaGL uniform buffers` (copies and KiB not copied a frame), `[fastcmp]` (UNICMP blocks compared). Pass rule: no new [hitch], no fault, the picture as in the base run."
RUNS += [
    _r17("be26on-v", "be26 base (UNICMP=1, UNIHIST=1 as planned): Vice goal scenario", "gsvice", BE26_BASE,
         "The planned build at the Vice goal (play, cut-scene, replay). Against be26off-v: `uniforms` and `issue` us per draw and be cpu." + BE26_WHY, group="be26"),
    _r17("be26off-v", "be26 A/B: both off (MSC_VGX_UNICMP=0, MSC_VGX_VGL_UNIHIST=0), Vice goal scenario", "gsvice", dict(BE26_BASE, MSC_VGX_UNICMP="0", MSC_VGX_VGL_UNIHIST="0"),
         "As be26on-v with the s28 behaviour: `uniforms` us per draw expected 1-3 us higher (a second copy of every block read), `issue` 0.3 us higher (the ring copies the history names again are made); cut-scene be cpu 0.5-1 ms higher. Estimates." + BE26_WHY, group="be26"),
    _r17("be26on-r", "be26 base (as planned): goal, celebration and replay route", "crep", BE26_BASE,
         "As be26on-v on the replay-heavy route (be cpu 20 / wall 28-33 in the q5 log)." + BE26_WHY, group="be26"),
    _r17("be26off-r", "be26 A/B: both off, goal, celebration and replay route", "crep", dict(BE26_BASE, MSC_VGX_UNICMP="0", MSC_VGX_VGL_UNIHIST="0"),
         "As be26off-v on the replay route." + BE26_WHY, group="be26"),
    _r17("unihist0", "be26 A/B: only the copy history off (MSC_VGX_VGL_UNIHIST=0)", "gsvice", dict(BE26_BASE, MSC_VGX_VGL_UNIHIST="0"),
         "Against be26on-v: `[vgx] vitaGL uniform buffers` copied KiB per frame higher by the history column (about 100 KiB in the cut-scene, estimate), `issue` us per draw higher." + BE26_WHY, group="be26"),
    _r17("unicmpv", "be26 verify: the uniform compare made both ways (MSC_VGX_UNICMP=2, MSC_VGX_VGL_UNIHIST=2), short check", "gsvice", dict(BE26_BASE, MSC_VGX_UNICMP="2", MSC_VGX_VGL_UNIHIST="2"),
         "`[fastcmp]` must end with 0 disagreed with the shadow, and `[vgx] vitaGL uniform buffers` must end with 0 reuses that differed from the buffer. Not a timing run (reads every reused ring copy back)." + BE26_WHY, group="be26"),
]

# helper f45 (w8/f45.md): the 45 cadence alone (MSC_CADENCE45=1, np_fps45.h), a developer switch: no Settings choice (owner 18 Nov); Smooth 60 in tiers uses it as its middle tier. The two measured runs and the owner's look.
F45_WHY = (" Pass rule: the log has `[f45] 45 cadence on` and `[f45] 300 pictures` lines (INVALID without). In live play: `off the pattern 0`; `[present]` held 1/2/3+ about 2 : 1 : 0; pattern slips under 8 %"
           " of the 45 pictures (late + held to a later predicted slot); interpolation error: pictures off their show vblank only behind slips (mean under 0.05 refreshes); no FALLBACK line at Vice;"
           " at most one fallback per match and never back and forth; `[speed]` 98 % or better; `[lat]` pad->shown p50 within 10 ms of the 30 FPS run (d30). Held scenes at 30 as in d30.")
TIER_WHY = (" Pass rule: the log has `[tier] Smooth 60 in tiers` and `[tier] 300 live pictures` lines (INVALID without). 60<->30 changes 0; no more than 2 tier changes in any 30 s; at the 45 tier"
            " `[f45]` off the pattern 0; `[speed]` 98 % or better; held scenes as in a Smooth 60 run without tiers.")
RUNS += [
    _r17("f45-v", "the 45 cadence alone (MSC_CADENCE45=1) with the headroom line, Vice tour", "tour", {"MSC_CADENCE45": "1", "MSC_HEAD_STAT": "1"},
         "f45: live play at 45 on the 1, 1, 2 pattern. Read `[f45]` (holds, slips, late, off the pattern, phase taken, interpolation error, L, the lead and its changes, room judged, gate waits, fallback with both windows), `[present]` held 0/1/2/3, `[head]` play with a step p50 / p95 against 22.2 ms, `[lane]` GPU, `[vgx] dynamic resolution` sizes (864x488 pixels or more: 960/888/832/784x544 only)." + F45_WHY, group="f45"),
    _r17("f45-d", "the 45 cadence alone (MSC_CADENCE45=1) with the headroom line, The Dump", "worstgpu", {"MSC_CADENCE45": "1", "MSC_HEAD_STAT": "1"},
         "f45: as f45-v at the heaviest stadium (GPU 20-30 ms at full size: the ladder should settle at 784x544-832x544 against the 22.2 ms budget). A fallback to 30 here is allowed but must be one, sticky, with the reason in the FALLBACK line." + F45_WHY, group="f45"),
    _r17("f45-l4", "45 FPS with the start fixed 4 refreshes before the slot (MSC_F45_LEAD=4), Vice tour", "tour", {"MSC_CADENCE45": "1", "MSC_F45_LEAD": "4", "MSC_HEAD_STAT": "1"},
         "f45 (Fable's review): the lead A/B against f45-v. f45-v learns the lead (starts at 5 = 83 ms start to picture; its `[f45] lead 5 -> 4` line, if any, and `room: N of M judged` say whether the console has a refresh to spare); this run fixes 4 (67 ms). Compare `late`, `pattern slips` and `[lat]` pad->shown p50 between the two: if f45-l4 has no more late pictures than f45-v, 4 is safe at Vice and the learned lead should have found it." + F45_WHY, group="f45"),
    _r17("s60t-v", "Smooth 60 in tiers (MSC_LOCK60=1; the tiers are its default), Vice tour", "tour", {"MSC_LOCK60": "1", "MSC_HEAD_STAT": "1"},
         "tiers (Fable): Smooth 60 defends 60, falls to the strict 45 on a sustained shortfall, to the full-size 30 only when 45 cannot hold. Read the `[tier]` lines: time at 60 / 45 / 30, tier changes by kind (60<->30 must be 0; no more than 2 changes in any 30 s), repeated refreshes at 60 and how many were tolerated, the 60 clock's pictures off their vblank; `[tier] a -> b` lines for when; `[f45]` lines for the 45 stretches. Compare with a Smooth 60 run without tiers (time at 30 there is time at 45 here, if the tiers work)." + TIER_WHY, group="f45"),
    _r17("s60t-d", "Smooth 60 in tiers (MSC_LOCK60=1; the tiers are its default), The Dump", "worstgpu", {"MSC_LOCK60": "1", "MSC_HEAD_STAT": "1"},
         "tiers (Fable): as s60t-v at the heaviest GPU stadium: expect little 60, the 45 at the ladder's floor or one sticky fall to the 30; the question is time per tier and that it never flaps." + TIER_WHY, group="f45"),
    _r17("s60t-v0", "Smooth 60 in tiers without the show-time clock at 60 (MSC_LOCK60_SHOWCLOCK=0), Vice tour", "tour", {"MSC_LOCK60": "1", "MSC_LOCK60_SHOWCLOCK": "0", "MSC_HEAD_STAT": "1"},
         "tiers (Fable): the A/B of the 60 tier's game time against s60t-v: same tiers, 60's time as before. Look (motion at 60 around repeated refreshes) and `[lat]` pad->shown at 60 (the show clock may hold pictures a refresh: p50 must not be more than 17 ms over this run's)." + TIER_WHY, group="f45"),
    _r17("s60t-off", "Smooth 60 without the tiers (MSC_LOCK60_TIERS=0: as before), Vice tour", "tour", {"MSC_LOCK60": "1", "MSC_LOCK60_TIERS": "0", "MSC_HEAD_STAT": "1"},
         "tiers (Fable): the OFF side of s60t-v: Smooth 60 as it was (3 misses of 60 -> the full-size 30 for 25 s or more). Compare `[fps]` time at 60 / 30 and the switches with s60t-v's `[tier]` time at 60 / 45 / 30.", group="f45"),
    _r17("f45-look", "45 FPS for the owner to watch: a match of goals, no diagnostics", "replay", {"MSC_CADENCE45": "1"},
         "f45: the owner's look at 45 (no diagnostic switch beyond the kit's defaults): is play smooth and even, are goals, celebrations and replays (at 30) clean at the cut, does it ever flap between 45 and 30. The `[f45]` lines still say what happened." + F45_WHY, group="f45"),
]

# logs16: a bigger log budget for the hitch lines in the block A runs and the goal-scenario runs (log volume only; the writer is a ring buffer with its own thread)
HITCH_LINES_IDS = ("d1", "worstgpu1", "crep1", "smooth0", "dis-on-a", "dis-on-b", "dis-on-c", "dis-off-a", "dis-off-b", "dis-off-c", "ram112-dump", "ram112-a", "rehearse6", "loadhold1",
                   "scenepin1", "scenepin1-2", "nodepth1", "heldaa13", "heldaa13w2", "heldaa13w2-2", "cutlearn2", "cutlearn2-2", "gsvice16", "gsdump16", "gslava16",
                   # r17 (kit20): the 60 answer and the simt runs of block A, the 60-mode PGO runs, the shimmer and lock60x runs
                   "lock60tour", "lock60pipe", "lock60dump", "lock60lava", "lock60gs-v", "lock60gs-d", "simt-blk-v", "simt-v", "simt-d", "simt-lava", "simt30-d", "simt-c1", "simt-c12", "simt-fs12", "simt-c3", "simt-lat",
                   "dlweld0", "simt-pgo", "pgo-base-lock60", "pgo-on-lock60", "pgox-lock60", "lock60x-dump", "lock60x-lava", "warboff-l", "warb-l", "warbl-s")
for _r in RUNS:
    if _r["id"] == "dlweld0":
        _r["why"] += (" r17 pass rule (fable-da21 F7): the weld is ON in worstgpu1 and every block A / B run; the `[dlweld]` line must be present in worstgpu1 (INVALID if missing: the weld did not run), there must be no new [hitch] at the walk-out / goal cuts of worstgpu1 against this run, and no fault or broken geometry anywhere. "
                      "Keep the weld on unless the on side is slower or faults; a fault or odd stadium geometry in any run of the sitting is read against this run first.")
    if _r["id"] == "warboff-l":
        _r["why"] += (" r17 pass rule (fable-da21 F6): in lock60lava and gslava16 the `[acts]` line (every 3600 pictures, the first at picture 600: take the line at picture 4200 of gslava16 when the first is still in the walk-out) must show `EFBSWAP_NEXT` swaps and `CLEARVEH` clear-only copies above 0 (a 0 means the cut did nothing on the console, as PASSISO_COVER did nothing in the emulator); no `[efbswap] ... little video memory` and no new FAILED lines in gslava16; `[lane] gpu` and late % of lock60lava not worse than this run.")
    if _r["id"] == "lock60lava":
        _r["why"] += " The shimmer cuts are ON (plan default): read `[acts]` (EFBSWAP_NEXT swaps, CLEARVEH copies above 0) and compare with warboff-l, which follows it."
LOCK60_RULE = (" r17 pass rule (plan-wave17 section 3 + split17 + head17): p50 16.7 ms, late under 2 %, no relief in open play, `[head]` game-thread p95 under 14.5 ms, and `[split] held == finished, leaked 0` "
               "(INVALID if the `[head]` or `[split]` line is missing: MSC_HEAD_STAT=1 is in the env).")
for _r in RUNS:
    if _r["id"].startswith("lock60") and _r["id"] not in ("lock60gs-v", "lock60gs-d") and not _r["id"].startswith("lock60x") and ("MSC_HEAD_STAT", "1") not in _r["env"]:
        _r["env"].append(("MSC_HEAD_STAT", "1"))
        _r["why"] += LOCK60_RULE
for _r in RUNS:
    if _r["id"] in HITCH_LINES_IDS:
        _r["env"].append(("MSC_HITCH_LINES", "1500"))

# The two-Vita LAN section: two legs, each leg is one match on two consoles. The host decides the input delay.
LAN_LEGS = [
    dict(id="lan-base", title="LAN match, the game's own input delay", host_env=[], join_env=[]),
    dict(id="lan-auto", title="LAN match, MSC_LAN_INPUT_DELAY=auto (host)", host_env=[("MSC_LAN_INPUT_DELAY", "auto")], join_env=[]),
    # r17 (kit20): the overlapped match step in a session (plan-simthread17 section 5; needs the simfix20 package): same score, no desync message, `[mp-lan]` sync ticks 0 mismatches
    dict(id="lan-simt-both", title="LAN match, MSC_SIMT=2 on both consoles (MSC_SIMT_LAN=1)", host_env=[("MSC_SIMT", "2"), ("MSC_RENDER_RNG", "2"), ("MSC_SIMT_LAN", "1"), ("MSC_OVERLAP_LOG", "1")],
         join_env=[("MSC_SIMT", "2"), ("MSC_RENDER_RNG", "2"), ("MSC_SIMT_LAN", "1"), ("MSC_OVERLAP_LOG", "1")]),
    dict(id="lan-simt-host", title="LAN match, MSC_SIMT=2 on the host console only (MSC_SIMT_LAN=1)", host_env=[("MSC_SIMT", "2"), ("MSC_RENDER_RNG", "2"), ("MSC_SIMT_LAN", "1"), ("MSC_OVERLAP_LOG", "1")], join_env=[]),
]
LAN_COMMON = [("MSC_FRAME_TIMING", "300"), ("MSC_LAT", "1"), ("MSC_WATCH", "1"), ("MSC_WATCH_EVERY", "600")]
LAN_SCRIPTS = {"host": "lan_host.txt", "join": "lan_join.txt"}
# SAME WI-FI (MSC_LAN_MODE=wifi): both Vitas stay on the access point, so the Mac keeps its FTP link to both while they play. The ad hoc
# default (NEARBY) drops the Vita's Wi-Fi to the Mac and needs the system dialog; its roles are MSC_LAN_DIALOG=create / join (README.md).
LAN_ROLE_ENV = {"host": [("MSC_LAN_MODE", "wifi")], "join": [("MSC_LAN_MODE", "wifi")]}


def session_order(tier=1, staged=False, manual=False):
    """The runs of a session in order, as ('run', entry) and ('swap', version) steps; versions are visited in the order the entries sit in
    RUNS (the core session first, then 1.3, 1.2, 1.1, 1.0, and back to 1.3 for the closing reference runs). tier 0 is the core session alone;
    staged=True (session.sh full) runs tier by tier: the core, then tier 1, tier 2 and the add-ons, instead of the tiers interleaved in RUNS order.
    Runs whose role is "manual" (a plug-in set by hand) only with manual=True."""
    steps, current = [], "1.3"
    ordered = sorted(RUNS, key=lambda r: r["tier"]) if staged else RUNS     # (sorted() is stable: RUNS order within a tier)
    for r in ordered:
        if r["tier"] > tier or (r["role"] == "manual" and not manual):
            continue
        if r["version"] != current:
            steps.append(("swap", r["version"]))
            current = r["version"]
        steps.append(("run", r))
    return steps


def estimate(tier=1, with_lan=True, measured=None, staged=False):
    """Console minutes of a session: (total, list of (label, minutes))."""
    measured = measured or {}
    rows = [("setup", SETUP_MIN)]
    for kind, item in session_order(tier, staged):
        if kind == "swap":
            rows.append(("swap to " + item, SWAP_MIN))
        else:
            rows.append((item["id"], measured.get(item["id"], minutes(item))))
    if with_lan:
        for leg in LAN_LEGS:
            rows.append((leg["id"], LAN_LEG_MIN))
    return sum(m for _, m in rows), rows


def core_blocks(measured=None):
    """The core session by block: [(block, minutes, [run ids])], a swap counted in the block it opens."""
    measured = measured or {}
    blocks, pending = [["setup", SETUP_MIN, []]], 0.0
    for kind, item in session_order(0):
        if kind == "swap":
            pending += SWAP_MIN
            continue
        if blocks[-1][0] != item["block"]:
            blocks.append([item["block"], pending, []])
            pending = 0.0
        blocks[-1][1] += measured.get(item["id"], minutes(item))
        blocks[-1][2].append(item["id"])
    return [tuple(b) for b in blocks]


# ---- wave 15 (ab15, ab15b): the in-run A/B runs. ONE run flips up to six switches a, b, a, b ... inside the run (MSC_AB, renderer/src/vgx_ab.h; docs/TECHNICAL.md), so heat, battery and card drift cancel
# and no repeat pairs are needed. Read with `abpair.py --inrun <log>` (b windows against the a windows beside them; a neighbour of another scene is left out). The value has NO colon (the test build
# refuses ':' in a value). a = the plan's value (the baseline), b = the candidate; a key's block is a (b a) x MSC_AB_REPEATS windows of 6 s after MSC_AB_START_S seconds of play, one key after the other.
# The estimates below are the route's minutes: the plan has to fit in the route's open play, and a block the route ends in has fewer b windows (the `[ab] plan finished` line is then missing).
AB_COMMON = {"MSC_AB_START_S": "40", "MSC_AB_REPEATS": "3"}
AB_WHY = " Read: abpair.py --inrun LOG (key, metric, diff b minus a, 90 % interval, verdict); the `[ab] flip` lines show the clock of each flip, `[lane] ... ab KEY=v a wN` the windows. Replaces the pairs listed in w8/ab15.md and w8/ab15b.md."
RUNS += [
    _r17("ab-vice60", "In-run A/B at Vice, LOCKED 60: packet-refresh prefetch, back-end prefetch, mid-scene flush, no-write draws", "tour",
         dict(GS60, MSC_GSPLIT_DETAIL="0", MSC_AB="MSC_FS_REFRESH_PREFETCH,1,0;MSC_VGX_BEPREFETCH,0,3;MSC_VGX_MIDFLUSH,0,30003;MSC_VGX_PX_NOOP,1,0", **AB_COMMON),
         "Replaces fsrp-on/off x2, refreshpf0, bepf2 and bepf4 (31.9 min). `game` (lane) for MSC_FS_REFRESH_PREFETCH (was 0.6-0.9 ms of the game thread), `be cpu` and fps for BEPREFETCH and MIDFLUSH, `gpu` and fps for MSC_VGX_PX_NOOP; a `[vgx] midflush:` line with no fault." + AB_WHY,
         group="ab15"),
    _r17("ab-dump", "In-run A/B at The Dump (GPU-bound): the pixel-work cuts and the mid-scene flush", "worstgpu",
         dict(MSC_AB="MSC_VGX_PX_SCISSOR,1,0;MSC_VGX_PX_NOOP,1,0;MSC_VGX_PX_DEADCLEAR,1,0;MSC_VGX_MIDFLUSH,0,30003", **AB_COMMON),
         "Replaces pxscissor0, pxcut0 and midflush-dump (12.8 min). `gpu` and fps of each pixel cut where the GPU is the limit (20-30 ms); MIDFLUSH is the control (expected no gain)." + AB_WHY, group="ab15"),
    _r17("ab-rep", "In-run A/B in the replay route: mid-scene flush, shadow redraw every 2nd frame, far players with the open-play mesh", "replay",
         dict(MSC_AB="MSC_VGX_MIDFLUSH,0,30003;MSC_SHADOW_EVERY,1,2;MSC_REPLAY_FARLOD,0,12", MSC_AB_START_S="20", MSC_AB_REPEATS="4"),
         "Replaces midflush-replay and replay2 (6 min), shadow2 and farlod12 (12 min). Only the windows inside a replay or cut-scene can show the shadow and far-LOD keys (the game reads them there only): the draw-count guard of abpair leaves out neighbours of another scene, so read `n` and expect 3-6 usable b windows a key; where it says too few windows, run `farlod12` / `shadow2` as before." + AB_WHY,
         group="ab15"),
    _r17("ab-vgl", "In-run A/B of the vitaGL switches that are on in the plan: exact viewports, scissor-mask keep, mask background, fragment memo, uniform reuse", "tour",
         dict(MSC_AB="MSC_VGX_VP_EXACT,1,0;MSC_VGX_VGL_MASKKEEP,1,0;MSC_VGX_VGL_MASKBG,3,1;MSC_VGX_VGL_FRAGMEMO,1,0;MSC_VGX_VGL_UNIREUSE,1,0", **AB_COMMON),
         "Replaces vpexact0, maskkeep0, maskbg0 and gal0 (19.6 min; maskbg1, the value 1 against the default 3, stays a single run). The flips are made by the render thread at a scene boundary (Vita only: nothing of this ran off the console, so a fault or a [vgx] line that is not as before ends the run's use and the five single runs stay). `gpu` and fps; vpexact is a look question as well (the 1-px line): take a screenshot run if it moves." + AB_WHY,
         group="ab15", tier=2),
]


# ---- wave 16 (final16): the console lines of the wave 13-15 helpers that had no kit id (w8/VITA-BATCH-1122.md orders them for ONE sitting). The ids are the helpers' own tags. Env only;
# the package of a run (plain / PGO / ordered PGO) is chosen by the batch, not by the env file. A run with role `manual` is not part of `full` (session.sh addon).
HS = {"MSC_HEAD_STAT": "1"}
BE24 = {"MSC_GSPLIT": "2", "MSC_VGX_BEPH": "1", "MSC_VGX_GXM_DIRECT": "1"}
MVN = {"MSC_THP_NOWAIT": "1"}
RUNS += [
    # ---- PGO A/B (pgo16): only the eboot differs; the batch alternates A B B A A B with abpair --paired ----
] + [
    _r17("pgo16-%s-%s-%s" % (scn, side, n), "PGO16 A/B at %s play: %s, run %s (only the eboot differs between the sides)" % ("Vice" if scn == "vice" else "The Dump", label, n),
         "tour" if scn == "vice" else "worstgpu", dict(HS, MSC_GSPLIT="2"),
         "pgo16.md: accept plain -> PGO: 1-step game thread p50 down >= 0.8 ms; PGO -> ordered PGO: down >= 0.3 ms; no new fault in 60 s. Read the game thread/step p50 and the `[gsplit-pic]` 1-step row at play. The batch alternates the packages A B B A A B (3 pairs); compare with `abpair.py --paired`. Package: %s." % pkgname,
         group="w16")
    for scn, side, label, pkgname in (("vice", "plain", "plain eboot", "s35-plain"), ("vice", "pgo", "PGO eboot", "s35-pgo"), ("vice", "pgoref", "PGO eboot (reference of the order pair)", "s35-pgo"),
                                      ("vice", "order", "PGO eboot with the hot-order link", "s35-pgo-order"), ("dump", "plain", "plain eboot", "s35-plain"), ("dump", "pgo", "PGO eboot", "s35-pgo"))
    for n in "abc"
] + [
    _r17("texc-base", "Dump baseline right before the texture compression runs (same env as texc-ceil without the key)", "worstgpu", dict(HS), "texc15b.md: the plain Dump baseline the texc runs are read against (run just before texc-ceil, same session).", group="w16"),
    # ---- replay / walk-out (walk15): the crowd cluster flag ----
    _r17("walk-cf0", "Walk-out and goal cut-scene, A side (defaults), Vice; its first two [lane] windows also read the kick-off (kick-d1)", "tour", dict(HS, MSC_CROWD_CLUSTERFLAG="0"),
         "walk15.md: A side of the crowd cluster flag: `[lane]` gpu / fe / be in the walk-out window and the goal windows, `[head]` cut-scene p50, `[hitch]` counts. The first two `[lane]` windows after kick-off: fps against r11's 26-28 (Vice).", group="w16"),
    _r17("walk-cf1", "Walk-out and goal cut-scene with MSC_CROWD_CLUSTERFLAG=1 (visible: far crowd rows animate at half rate), Vice", "tour", dict(HS, MSC_CROWD_CLUSTERFLAG="1"),
         "walk15.md (host: walk-out passes 36.1 -> 20.7, goal cut-scene 33.2 -> 25.6; ESTIMATE -3 to -6 ms GPU): B side, same lines as walk-cf0 and the passes/copies in the walk-out census line. Look at the crowd: far rows animate at half rate, near rows and the players unchanged. Decides a default (visible).", group="w16"),
    _r17("walk-cf0d", "Walk-out and goal cut-scene, A side (defaults), The Dump", "worstgpu", dict(HS, MSC_CROWD_CLUSTERFLAG="0"), "walk15.md: the A side of walk-cf1d at the worst GPU stadium (cut-scene fps was 19-21 in r10).", group="w16"),
    _r17("walk-cf1d", "Walk-out and goal cut-scene with MSC_CROWD_CLUSTERFLAG=1, The Dump", "worstgpu", dict(HS, MSC_CROWD_CLUSTERFLAG="1"), "walk15.md: against walk-cf0d. Same lines as walk-cf1.", group="w16"),
    _r17("walk-cf3", "Crowd cluster flag with a floor of 3 (MSC_CROWD_REPLAY=3), Vice", "tour", dict(HS, MSC_CROWD_CLUSTERFLAG="1", MSC_CROWD_REPLAY="3"),
         "walk15.md: ONLY if walk-cf1 leaves the walk-out over 33.3 ms: the crowd every 3rd picture at 30.", group="w16", tier=3),
    _r17("walk-fs2", "Walk-out / cut-scene frames sent on the game thread (MSC_FS_HELD=2), Vice", "tour", dict(HS, MSC_FS_HELD="2"),
         "walk15.md: rep27's switch in walk-outs and cut-scenes (the end-frame send was 27.5 ms in the walk-out, gsplit1): read the game / send / fe lanes. Low expectation: replays were no faster in rep27.", group="w16", tier=3),
    # ---- vbuf (vbuf15): the cached stream arena, against bephbase / the replay route ----
    _r17("vbuf-v", "Cached vertex stream arena (MSC_VITA_VBUF_CACHED=1) at the goal scenario, Vice", "gsvice", dict(BE24, MSC_VITA_VBUF_CACHED="1"),
         "vbuf15.md: against `bephbase` (same env without the key): the `[vbuf]` line, back-end CPU ms a picture, `[lane]` be CPU. Wrong pictures or a fault = stop.", group="w16"),
    _r17("vbuf-r", "Cached vertex stream arena (MSC_VITA_VBUF_CACHED=1) on the replay route", "crep", _m(BE24, HS, {"MSC_VITA_VBUF_CACHED": "1"}),
         "vbuf15.md: the same on the replay route, against vbuf-r0.", group="w16"),
    _r17("vbuf-r0", "The replay route with the back-end phase split on, no cached arena (control of vbuf-r)", "crep", _m(BE24, HS), "vbuf15.md: control of vbuf-r.", group="w16"),
    # ---- movies (movie14, fable-review-movie14, walk15) on the ntour route (stadium previews) ----
    _r17("mv-base", "Movies: stadium previews at the defaults (read first)", "ntour", dict(MVN, MSC_VGX_I8CHURN="0"),
         "movie14.md: the `[thp-pace]` line of every stadium preview: repeated, late, doubled, longest gap, `recovered ... 0`, `lanes given up 0`; does any repeat exist? If `repeated 0` and `late 0` for all kinds, drop the delay line.", group="w16"),
    _r17("mv-d2", "Movies: MSC_THP_DELAY=2 (the picture 66 ms behind its sound)", "ntour", dict(MVN, MSC_THP_DELAY="2"),
         "movie14.md: the same lines at delay 2: do repeated and late fall to about 0? Watch the previews for lip-sync of sound against picture. Timing change: decides a default.", group="w16"),
    _r17("mv-d3", "Movies: MSC_THP_DELAY=3", "ntour", dict(MVN, MSC_THP_DELAY="3"), "movie14.md: ONLY if mv-d2 still shows repeats.", group="w16", tier=3),
    _r17("mv-sw", "Movies: software decode of the previews (MSC_THP_HWJPEG=0)", "ntour", dict(MVN, MSC_THP_HWJPEG="0"), "movie14.md: the repeat / late numbers the codec is hiding (a control).", group="w16", tier=3),
    _r17("mv-i8", "Movies: one byte a texel for the preview planes (MSC_VGX_I8CHURN=1)", "ntour", dict(MVN, MSC_VGX_I8CHURN="1"),
         "walk15.md: against mv-base: the front end ms and the `[vgx-vita] ... texture uploads ... as one byte a texel` line at the stadium select; `[thp-pace]` repeated / late; the preview's greys (a wrong grey = stop); fe was 22 ms.", group="w16"),
    _r17("mv-intro", "Movies: the opening, defaults (MANUAL: delete the MSC_INPUT_SCRIPT line, touch no button)", "cintro", dict(MVN),
         "movie14.md: boot to the title and let the opening play without touching the pad; read the `[thp-pace]` line of introtest. The route script would press A and skip it, so remove the MSC_INPUT_SCRIPT line from env.txt for this run.", group="w16", role="manual"),
    _r17("mv-intro2", "Movies: the opening at MSC_THP_DELAY=2 (MANUAL as mv-intro)", "cintro", dict(MVN, MSC_THP_DELAY="2"),
         "movie14.md: the heavy movie under delay 2. Same manual procedure as mv-intro.", group="w16", role="manual"),
    _r17("mv-cred", "Movies: the credits (MANUAL: Extras / Options -> Credits, let it run), defaults", "cintro", dict(MVN),
         "movie14.md: nlgintro + credits.thp, one `[thp-pace]` line each. Remove the MSC_INPUT_SCRIPT line.", group="w16", role="manual"),
    _r17("mv-cred2", "Movies: the credits at MSC_THP_DELAY=2 (MANUAL as mv-cred)", "cintro", dict(MVN, MSC_THP_DELAY="2"), "movie14.md: the same at delay 2.", group="w16", role="manual"),
    _r17("mv-ps", "Movies: press the PS button during the opening and during a stadium preview, then resume (MANUAL)", "cintro", dict(MVN),
         "fable-review-movie14.md: how the Vita behaves on suspend with a codec call in flight. Press PS during the opening, resume, then at a stadium preview, resume; the picture and sound must continue. Look for `has not returned` in the log, `[thp-hw]` lines. Remove the MSC_INPUT_SCRIPT line.", group="w16", role="manual"),
    # ---- the fourth core (core3x15): ONLY with the CapUnlocker plug-in installed (reboot first); no kernel code in the game ----
    _r17("c3w-off", "Fourth core off (control), LOCKED 60 split lines, Vice", "tour", dict(GS60, MSC_VITA_CORE3="0"), "core3x15.md: control: `[gsplit-pic]` 1-step p50 / p95, the `[jobs]` line. Needs CapUnlocker for the others; without it every c3w line must log `fourth core: not available` and behave as this one.", group="w16"),
    _r17("c3w-j", "Fourth core: one more job lane (MSC_VITA_CORE3_WORK=1), Vice", "tour", dict(GS60, MSC_VITA_CORE3="1", MSC_VITA_CORE3_WORK="1", MSC_JOBS="3"),
         "core3x15.md: `fourth core: usable`, `[jobs] MSC_VITA_CORE3_WORK=1: lane 3`, `core 3 lane: N calls, M late`, no `given up`; game ms against c3w-off. NEEDS CapUnlocker.", group="w16"),
    _r17("c3w-f", "Fourth core: front-end decode worker (MSC_VITA_CORE3_WORK=2), Vice", "tour", dict(GS60, MSC_VITA_CORE3="1", MSC_VITA_CORE3_WORK="2"),
         "core3x15.md: `decode worker ... on the system core`, `[fepar]` worker busy / join wait, no `kept the player waiting`. NEEDS CapUnlocker.", group="w16"),
    _r17("c3w-all-d", "Fourth core: both (MSC_VITA_CORE3_WORK=3), LOCKED 60 split lines, The Dump", "worstgpu", dict(GS60, MSC_VITA_CORE3="1", MSC_VITA_CORE3_WORK="3"),
         "core3x15.md: the same at The Dump. NEEDS CapUnlocker.", group="w16"),
    _r17("c3w-rep", "Fourth core: both (MSC_VITA_CORE3_WORK=3) on the replay route", "crep", dict(HS, MSC_VITA_CORE3="1", MSC_VITA_CORE3_WORK="3"),
         "core3x15.md: replay pictures p50 against fsheld-on / crep1 without it (core 1 bound). NEEDS CapUnlocker.", group="w16"),
    _r17("c3w-on-online", "Fourth core (MSC_VITA_CORE3_WORK=3): open the online hub (MANUAL)", "onhub", {"MSC_VITA_CORE3": "1", "MSC_VITA_CORE3_WORK": "3"},
         "core3x15.md: open the online hub: threads leave core 3 (the guard line) and come back 3 s later. NEEDS CapUnlocker.", group="w16", role="manual"),
    _r17("gxnet14-pace", "Client obeys the service's pacing hints (MSC_NET_PACE_HINTS=1) under a busy server (MANUAL, online)", "onconn", {"MSC_NET_PACE_HINTS": "1"},
         "gxnet14.md: does the client obey nextWaitSec / nextPollMs under a busy server? Read the request-spacing lines in the log. Decides a default (timing).", group="w16", role="manual"),
    _r17("gxnet14-radio", "Local Wireless after a previous session (MSC_LAN_RADIO_FIX=1, soft close) (MANUAL, LAN pair)", "onconn", {"MSC_LAN_RADIO_FIX": "1"},
         "gxnet14.md: does Local Wireless start after a previous session without restarting the game? Hardware behaviour: decides a default.", group="w16", role="manual"),
    # ---- Wi-Fi keep-awake (wake14): a PC partner (the emulator / host32 console) on the same Wi-Fi; the Vita side matters ----
    _r17("wakegap-off", "Wi-Fi gaps measured (MSC_LINK_GAPLOG=50), keep-awake off (MANUAL: LAN pair with the PC partner)", "onconn", {"MSC_LINK_GAPLOG": "50"},
         "wake14.md: which phases go quiet: play lobby, draft, load, kick-off, one goal, one Mega Strike, one replay, results, rematch; read the `[vlink] gap table` lines on the Vita and on the PC partner (the partner's rx gaps are the Vita's real tx cadence). NEEDS A PC PARTNER.", group="w16", role="manual"),
    _r17("wakegap-on", "Wi-Fi gaps with the keep-awake filler (MSC_NET_KEEPAWAKE_MS=50) (MANUAL: LAN pair with the PC partner)", "onconn", {"MSC_LINK_GAPLOG": "50", "MSC_NET_KEEPAWAKE_MS": "50"},
         "wake14.md: on both consoles, then once on the Vita only: tx longest <= ~100 ms outside stalls; the partner's rx table should show no Vita burst after a quiet spell; the same goal / Mega Strike / replay: `[mp-lan] net` window after each cut-scene (starved=, rtt= max). Decides a default. NEEDS A PC PARTNER.", group="w16", role="manual"),
    _r17("wakebat-on", "Keep-awake battery cost: MSC_NET_KEEPAWAKE_MS=50 parked 10 min in the connected lobby (MANUAL)", "onconn", {"MSC_NET_KEEPAWAKE_MS": "50"},
         "wake14.md: battery percent after 10 minutes against the same with =0 (the default run is wakegap-off without the log key); same start percent, screen on, same brightness. NEEDS A PC PARTNER.", group="w16", role="manual"),
    _r17("wakebat-off", "Keep-awake battery control: parked 10 min in the connected lobby, keep-awake off (MANUAL)", "onconn", {"MSC_NET_KEEPAWAKE_MS": "0"}, "wake14.md: control of wakebat-on. NEEDS A PC PARTNER.", group="w16", role="manual"),
    # ---- texture compression (texc15b): LAST; delete ux0:data/mscharged/texcomp.idx and .dat before texc-1 (and again afterwards) ----
    _r17("texc-ceil", "Texture compression ceiling (MSC_VGX_TEX_COMPRESS=ceiling): the price of the best case, wrong pictures on purpose", "worstgpu", dict(HS, MSC_VGX_TEX_COMPRESS="ceiling"),
         "texc15b.md PRICE: `[lane]` gpu p50 / p95 and 3D ms against plain worstgpu1 at the Dump in play; `[texcomp]` eligible MiB and `LEFT FOR THE BYTE CAP OR HEAP FLOOR`, REFUSED UPLOADS. Under 0.3 ms: stop (no texc-1/2/r); over 1 ms: go on.", group="w16"),
    _r17("texc-1", "Texture compression on, cold card", "worstgpu", dict(HS, MSC_VGX_TEX_COMPRESS="1"),
         "texc15b.md: delete texcomp.idx and .dat first. `[texcomp]` replaced MiB -> MiB, encode ms and worst picture (hitches in `[hitch]`), gate rejected, REFUSED UPLOADS, left for cap; look at stadium, cut-outs, glows / sky, HUD / menus; `[lane]` gpu. Do not judge smoothness from this run.", group="w16"),
    _r17("texc-2", "Texture compression on, warm card (the real gain)", "worstgpu", dict(HS, MSC_VGX_TEX_COMPRESS="1"),
         "texc15b.md: `(N from the card)`, encoded 0, `[lane]` gpu p50 / p95 against worstgpu1 = the real gain. Over 0.3 ms GPU p50 with no visible change: candidate for default ON.", group="w16"),
    _r17("texc-r", "Texture compression on the replay route", "crep", dict(HS, MSC_VGX_TEX_COMPRESS="1"), "texc15b.md: the same on the replay route (0 to 1 ms expected).", group="w16"),
    # ---- other wave 13-15 lines ----
    _r17("texcopy-0", "Stage load at The Dump, baseline (texture copy by CPU)", "worstgpu", dict(HS), "gxvita14.md: baseline stage load time (the `[texcopy]` line is absent).", group="w16", tier=3),
    _r17("texcopy-1", "Stage load at The Dump with the DMA texture copy (MSC_VITA_TEXCOPY=1)", "worstgpu", dict(HS, MSC_VITA_TEXCOPY="1"),
         "gxvita14.md: does the DMA copy shorten the stage load (ESTIMATE ~55 of ~90 ms texture copy); the `[texcopy]` line.", group="w16", tier=3),
    _r17("texcopy-2", "Stage load at The Dump, DMA copy checked against the CPU copy (MSC_VITA_TEXCOPY=2)", "worstgpu", dict(HS, MSC_VITA_TEXCOPY="2"), "gxvita14.md: the copies equal the CPU copy (differed 0).", group="w16", tier=3),
]
RUNS += [
    # ---- port17 (UDP ports below 32768) and gxp17 (on-console sampler .bin; NEEDS CapUnlocker) ----
    _r17("port17-online", "Online socket binds below 32768 (MANUAL: Online Central > Check Connection)", "onconn", {},
         "port17.md: read `[np-net] UDP port 25110 bound (asked 25110, 1 bind tried)` or a chain of `bind to UDP port N failed (0x...)` lines. s36 must not be used for this: it asked for 41000, which the Vita refuses.", group="w18", role="manual"),
    _r17("port17-lan", "Same Wi-Fi host start binds below 32768 (MANUAL: LAN with the PC partner)", "onconn", {},
         "port17.md: read `[vlink-vita] UDP port 25100 bound (asked 25100, 1 bind tried)` and `Wi-Fi link on ... port 25100`; the partner finds the Vita. NEEDS A PC PARTNER. s36 must not be used for this.", group="w18", role="manual"),
    _r17("samp-dump-play", "On-console sampler: The Dump, 2 minutes of open play after kick-off (NEEDS CapUnlocker)", "worstgpu", dict(HS, MSC_SAMPLER="1", MSC_SAMPLER_TAG="dump-play", MSC_SAMPLER_SECS="20"),
         "gxp17.md: copy the unstripped mscharged-np.elf of the SAME build and sampler-*.bin, `python3 -I tools/console-1.2/sampler_report.py mscharged-np.elf sampler-*.bin --top 40`: run/wait split, subsystems, top 40, `stopped it x %` (overhead, expect 2-3 %). NEEDS CapUnlocker.", group="w18"),
    _r17("samp-dump-replay", "On-console sampler: The Dump goal, celebration and the whole replay (NEEDS CapUnlocker)", "gsdump", dict(HS, MSC_SAMPLER="1", MSC_SAMPLER_TAG="dump-replay", MSC_SAMPLER_SECS="20", MSC_SAMPLER_THREADS="send"),
         "gxp17.md: the goal route scores at once; read the replay windows (2 minutes). MSC_SAMPLER_THREADS=send is a guess at the frame-send thread name: the log lists the live thread names at every window until found; change it to a part of the real name. Report as samp-dump-play. NEEDS CapUnlocker.", group="w18"),
    _r17("samp-dump-walkout", "On-console sampler: The Dump walk-out only (NEEDS CapUnlocker)", "gsdump", dict(HS, MSC_SAMPLER="1", MSC_SAMPLER_TAG="dump-walkout", MSC_SAMPLER_SECS="10"),
         "gxp17.md: the walk-out is about 18 s: .bin #1 and #2 only, then quit; the walk-out is the lane-bound scene (50 ms). NEEDS CapUnlocker.", group="w18"),
]
RUNS += [
    # ---- skinpar18 (w8/skinpar18.md): the characters' skin-mesh poses on the job lanes (MSC_SKIN_POSE_PAR, default 0 in the plan until the console says) ----
    _r17("ab-skinpose-d", "In-run A/B at The Dump, LOCKED 60: skin-mesh poses on the job lanes (MSC_SKIN_POSE_PAR 0 against 1)", "worstgpu",
         dict(GS60, MSC_GSPLIT_DETAIL="0", MSC_AB="MSC_SKIN_POSE_PAR,0,1", **AB_COMMON),
         "skinpar18: the console sampler of 23 Nov (The Dump match play) had ShaderSkinMesh::Pose and its callees at 8-10 % of the game thread; MSC_SKIN_POSE_PAR=1 makes the ten characters' skin poses on the job lanes before the draws (same bits: host det trace, frame hashes and a skin-mesh digest equal over the Vice and The Dump routes, verify counts 0 wrong, mutants caught). ONE run flips it 0, 1, 0 ... in 6 s windows. Read: abpair.py --inrun LOG: `game` (lane) and the [gsplit] character-draw ms (b should be lower by about 1 ms), fps; the `[skinpose]` lines (parallel= counts up in b windows only; lanes=3). Keep on only if the game thread is lighter and [hitch] / late % do not rise." + AB_WHY,
         group="w18"),
    _r17("ab-skinpose-v", "In-run A/B at Vice, LOCKED 60: skin-mesh poses on the job lanes (MSC_SKIN_POSE_PAR 0 against 1)", "tour",
         dict(GS60, MSC_GSPLIT_DETAIL="0", MSC_AB="MSC_SKIN_POSE_PAR,0,1", **AB_COMMON),
         "As ab-skinpose-d at Vice (the reference stadium)." + AB_WHY, group="w18"),
    _r17("skinpose-ver", "Skin-mesh poses on the job lanes with the console verify (MSC_SKIN_POSE_PAR=2)", "tour", dict(GS60, MSC_SKIN_POSE_PAR="2"),
         "skinpar18: as MSC_SKIN_POSE_PAR=1, and every listed pose is also recomputed on the game thread and compared bit for bit, every character's draw decision (visible, shock, cull, LOD) made again at its draw time and compared (slower: NOT a timing run). The `[skinpose]` line every 300 pictures must say mismatches=0 and decision_mismatches=0 with checked= and decisions= counting up; any other number means a lane computes something the game thread does not (floating-point mode, a shared scratch) or a draw changes what the next character's decision reads, and the switch stays 0.",
         group="w18"),
]
RUNS += [
    # ---- gt18 (w8/gt18.md): exact game-thread cuts of the 23 Nov console sampler (MSC_GT18_QUAT ON in the plan; MSC_PREFETCH bit 16 off) ----
    _r17("ab-gt18-v", "In-run A/B at Vice, LOCKED 60: quaternion blend without library calls (MSC_GT18_QUAT 0 against 1)", "tour",
         dict(GS60, MSC_GSPLIT_DETAIL="0", MSC_AB="MSC_GT18_QUAT,0,1", **AB_COMMON),
         "gt18: nlQuatNLerp / nlQuatRenormalize wrote their dot products and scales as calls into the SDK maths; with 1 they are the same expressions in place, no shared counters in NpWeightT (exact: host det trace and every frame hash identical over the Vice and The Dump routes, verify 25 M calls 0 mismatches, unit test 15 M quaternions, mutants caught). Host callgrind -5.4 % instructions of the whole process (-382 k a picture). ONE run flips it 0, 1, 0 ... in 6 s windows (nothing is carried between pictures: same bits both ways). Expected: game thread 0.1-0.2 ms lower in the b windows (ESTIMATE); keep it on unless b is slower." + AB_WHY, group="w18"),
    _r17("gt18off-d", "Quaternion blend through the library calls (MSC_GT18_QUAT=0; plan default 1), The Dump, LOCKED 60", "worstgpu", dict(GS60, MSC_GT18_QUAT="0"),
         "gt18: the off side for a plain A/B at The Dump (the in-run A/B ab-gt18-v is better). Same game output either way.", group="w18"),
    _r17("gt18ver", "Quaternion blend both ways with the console verify (MSC_GT18_QUAT=2)", "tour", dict(GS60, MSC_GT18_QUAT="2"),
         "gt18: both ways run, the original's answer is used and a difference is counted. The `[gt18] QUAT verify: checked N, mismatches M` line (every 262,144 calls) must say mismatches 0 with checked counting up; any other number means the new code gives other bits on the Vita's FPU and the plan key goes back to 0. Slower: NOT a timing run.", group="w18"),
    _r17("gt18pf-d", "Prefetch in NpSim26Apply (MSC_PREFETCH=16), The Dump, LOCKED 60", "worstgpu", dict(GS60, MSC_PREFETCH="16"),
         "gt18: console sampler 23 Nov had NpSim26Apply's callees at 2.0 % of the game thread (about 560 host instructions a bone: memory latency). Bit 16 starts the loads of the bone volumes and of the lane's per-bone results in the walk that counts the bones, and of the next bone's physics object and node matrix. A prefetch changes no value (host: det trace and frame hashes identical). A/B against gt18off-d's neighbour run without it (plan MSC_PREFETCH is 0): game ms and the sampler's NpSim26Apply share; keep it off unless it is faster.", group="w18"),
    # ---- hold60 (w8/hold60.md): Smooth 60 judged on the delivered rate, climbing back on measured work (MSC_LOCK60_HOLD, code default 0 until the console says) ----
    _r17("hold60-v", "Smooth 60 with the hold (MSC_LOCK60_HOLD=1): 60 kept down to 50 a second, climbs back on work, Vice tour", "tour",
         {"MSC_LOCK60": "1", "MSC_LOCK60_HOLD": "1", "MSC_HEAD_STAT": "1"},
         "hold60: the owner's 60. INVALID without `[hold60] on` and `[tier] 300 live pictures` lines. Read the last `[tier]` line: time at 60 / 45 / 30 (all so far) - PASS when 60 is at least 60 % of live play "
         "(the 23 Nov tiers: 2-3 %), 45 -> 60 changes > 0, and no two `[tier] a -> b` lines within 8 s other than the one-frame 60 -> 30 -> 60 at a cut into play; `[hold60]` delivered over 5 s >= 50 "
         "while at 60. Smoothness at 60 (owner 17:57): `[present]` judder <= 0.10 vblanks a picture in live play at 60, `[tier]` '60 with the show-time clock: N of M off the vblank' at most "
         "the repeated refreshes of that window, `[smooth]` players+ball motion minus shown p95 <= 1.0 and share over 0.5 <= 5 %. `[f45]` 'first input over' says which input fails the 30's "
         "45-fit test (the open question of 23 Nov). `[lat]` pad->shown p50 may be up to 17 ms over hold60-v0's (the 0.95 plan).", group="w18"),
    _r17("hold60-v0", "Smooth 60 tiers as before (MSC_LOCK60_HOLD=0), Vice tour: the A side of hold60-v", "tour",
         {"MSC_LOCK60": "1", "MSC_LOCK60_HOLD": "0", "MSC_HEAD_STAT": "1"},
         "hold60: the OFF side, same build, same route: compare the last `[tier]` time at 60 / 45 / 30 and changes with hold60-v (expect 60 under 5 %, as 23 Nov), `[present]` judder and `[lat]` pad->shown p50.", group="w18"),
    _r17("hold60-d", "Smooth 60 with the hold (MSC_LOCK60_HOLD=1), The Dump", "worstgpu",
         {"MSC_LOCK60": "1", "MSC_LOCK60_HOLD": "1", "MSC_HEAD_STAT": "1"},
         "hold60: the GPU-bound stadium (GPU 23-25 ms at full size). PASS: it must not force 60 where the pictures do not come: `[hold60]` delivered over 5 s >= 50 whenever `[tier]` says 60, "
         "60 -> 45 falls named in the `[fps] auto` lines with a hold60 reason, no two tier changes within 8 s, 45 (not 30) most of live play when the 45 holds (`[f45]` window late under 15 %).", group="w18"),
    # ---- hold60b (24 Nov): what the climb cost is made of, and the lever (the blend lane's wait). Read the second `[hold60]` line ("climb cost parts ... | at 60 ..."). ----
    _r17("h60b-v", "SMOOTH 60 defaults (hold, spread catch-up), Vice tour, with the split lines: the climb cost's parts and what the 60 measured", "tour", dict(GS60),
         "hold60b: INVALID without the second `[hold60]` line ('climb cost parts'). Read at 45: 'game thread X ms a picture (... of it the blend lane's wait Y)' (expect X 17-18, Y 3-4: s49), 'over N (P %; "
         "by game ..., of them only by the lane's wait ...)'; at 60 (Mega Strikes, cut-ins): 'at 60, n pictures: work W (of it the lane's wait V)' - the check of the estimate: a climb cost at 45 within "
         "1 ms of W measured at 60 in play. `[gsplit]` wait 'other' is the same lane wait. 'catch-up spread' counts. Time at 60 expected as s49 (Mega Strikes and cut-ins, ~20 % of live play).", group="w18"),
    _r17("h60b-sp0-v", "As h60b-v with the 60's catch-up not spread (MSC_LOCK60_SPREAD=0), Vice tour", "tour", dict(GS60, MSC_LOCK60_SPREAD="0"),
         "hold60b: the A side of h60b-v for the spread: `[present]` at 60 'game steps 1/2/3/4+ vblanks' and the [tier] 'repeated refreshes at 60' per window; with the spread (h60b-v) a late picture "
         "should cost one repeated refresh, here up to two.", group="w18"),
    _r17("h60b-ba0-v", "As h60b-v with the snapshot blend back on the job lanes (MSC_BLEND_ASYNC=0): the lever for 60, Vice tour", "tour", dict(GS60, MSC_BLEND_ASYNC="0"),
         "hold60b: THE question for 60 at Vice. The async blend's join blocks the game thread 3-4 ms a picture at 45 and 6-7 at a forced 60 (0.7 at the paced 30); gt27's A/B judged it by "
         "[gsplit-pic] work, which leaves that wait out. Compare with h60b-v: the second `[hold60]` line's game thread ms and lane wait at 45 (expect the lane wait ~0, the game thread lower by "
         "2-3 ms if the blend costs ~1 ms on the job lanes), `[gsplit-pic]` 1-step work (up ~1 ms), and the last `[tier]` time at 60 (PASS for the lever: 45 -> 60 changes > 0 and 60 above "
         "h60b-v's share with `[hold60]` delivered over 5 s >= 50 while at 60). Same game output either way (gt27: det and frame hashes identical).", group="w18"),
    _r17("f60-ba0-v", "Forced 60 (MSC_FPS=60, MSC_LOCK60=0) with MSC_BLEND_ASYNC=0, Vice tour: the 60's own cost without the lane's wait", "tour",
         {"MSC_FPS": "60", "MSC_LOCK60": "0", "MSC_BLEND_ASYNC": "0", "MSC_HEAD_STAT": "1", "MSC_GSPLIT": "2"},
         "hold60b: pairs with the s50 forced-60 run 5161 (live play 30.0 a second, [gsplit] wait 'other' 6.4-7.3 ms, `[late]` game 23.9). Read `[late]` game ms, `[gsplit]` 'other' (expect ~0) and "
         "steps a picture, `[present]` live-play hold mix and `[pace]` pictures a second: if the one-step picture now fits (13.4 + ~1 ms), live play runs at or near 60.", group="w18"),
    # ---- scenes30 (w8/scenes30.md): the held moments (walk-outs, goal celebrations, Mega Strike cut-scenes, replays) to a steady 30. MSC_HELD_GOV (the held-scene GPU governor: crowd floor,
    # shadow cadence, glow cadence, only while a held scene's GPU time is over 33.3 ms) and MSC_FX_GOV_HOLD=60 (the fill governor keeps its thinning between bursts) are ON in the plan; the
    # [head] line splits the held classes (walk-out, celebration, mega, mega-cut, replay, cut-scene) and counts their "over" against 33.4 ms. Pairs: run scn-on-X and scn-gov0-X back to back.
    _r17("scn-on-v", "Held moments with the held-scene governor and the fill governor's hold (defaults), Vice tour", "tour", dict(HS, MSC_HITCH_LINES="1500"),
         "scenes30: B side (plan defaults). Read per moment the `[head]` lines of walk-out, celebration, mega, mega-cut, replay: late % (target under 5 %: each picture shown 2 refreshes), GPU p95 and 'over 33.4 ms ... GPU' counts; "
         "`[heldgov]` lines (level moves, pictures at each level per scene, GPU mean at the end of each scene); `[vgx] fill governor` lines (moves to 0 while the GPU is over 33.4 ms should be gone: the console showed 18 in walk-outs before); "
         "`[present]` hold mix. Look: far crowd rows, players' shadow shapes and the glow haze in the walk-out and the goal celebration (they change only while the governor is engaged).", group="s30"),
    _r17("scn-gov0-v", "Held moments without the held-scene governor and with the old fill-governor rule (MSC_HELD_GOV=0, MSC_FX_GOV_HOLD=0), Vice tour", "tour",
         dict(HS, MSC_HITCH_LINES="1500", MSC_HELD_GOV="0", MSC_FX_GOV_HOLD="0"),
         "scenes30: A side of scn-on-v (the 23 Nov build's behaviour, now with the split [head] classes). Same lines; the difference in late % per moment is the answer. Run back to back with scn-on-v.", group="s30"),
    _r17("scn-on-d", "Held moments with the held-scene governor (defaults), The Dump", "worstgpu", dict(HS, MSC_HITCH_LINES="1500"),
         "scenes30: as scn-on-v at the heaviest stadium (walk-out GPU 42-48 ms, celebrations and Mega results 42-50 ms, replays 40-52 ms on 23 Nov). If walk-outs / celebrations still show late over 10 % "
         "with `[heldgov]` at level 3 most of the scene, the next step is scn-size3-d (the owner's call: below the floor).", group="s30"),
    _r17("scn-gov0-d", "Held moments without the held-scene governor (MSC_HELD_GOV=0, MSC_FX_GOV_HOLD=0), The Dump", "worstgpu",
         dict(HS, MSC_HITCH_LINES="1500", MSC_HELD_GOV="0", MSC_FX_GOV_HOLD="0"),
         "scenes30: A side of scn-on-d. Run back to back with scn-on-d.", group="s30"),
    _r17("scn-force3-d", "Held moments with every governor level in force from the cut (MSC_HELD_GOV_FORCE=3), The Dump", "worstgpu",
         dict(HS, MSC_HITCH_LINES="1500", MSC_HELD_GOV_FORCE="3"),
         "scenes30: the ceiling of levels 1-3 (crowd floor 3, shadows every 3rd, glow every 3rd) in every held picture, no climb. Against scn-gov0-d: GPU p95 per held class is what the three levels buy "
         "(host ESTIMATE -3 to -6 ms; passes a picture 24.6 -> 16.4 walk-out, 17.6 -> 11.8 celebration, 16.2 -> 10.8 replay). Not a default.", group="s30"),
    _r17("scn-size0-d", "Held moments with the size step off (MSC_HELD_GOV_SIZE=0: the 864x488 floor kept; plan default 3 since s49), The Dump: the A side of scn-on-d", "worstgpu",
         dict(HS, MSC_HITCH_LINES="1500", MSC_HELD_GOV_SIZE="0"),
         "scenes30 (owner 23 Nov 19:12 allowed the step; plan default 3 since s49): the OFF side. Compare late % of walk-out / celebration / replay against scn-on-d (console s48: celebrations 93 -> 7 % with the step).", group="s30", tier=3),
    _r17("scn-rep", "Replays with the held-scene governor (defaults), Vice crep route", "crep", dict(HS, MSC_HITCH_LINES="1500"),
         "scenes30: five goal sequences: the second and later replays start at the level the first learned (`[heldgov] replay begins at ... level L (what the last one of its kind spent most pictures at)`). "
         "Read [head] replay late % (23 Nov: 44-63 % late) and GPU p95, [heldgov] per replay.", group="s30"),
    _r17("ab-scn-rep", "In-run A/B in the replay route: held-scene governor and the fill governor's hold (MSC_HELD_GOV 0 against 1, MSC_FX_GOV_HOLD 0 against 60)", "replay",
         dict(MSC_AB="MSC_HELD_GOV,0,1;MSC_FX_GOV_HOLD,0,60", MSC_AB_START_S="20", MSC_AB_REPEATS="4", MSC_HEAD_STAT="1"),
         "scenes30: only the windows inside a replay or cut-scene can show these keys (the governor acts only there), so read `n` and expect few usable b windows; the pairs scn-on-v / scn-gov0-v are the main answer." + AB_WHY, group="s30", tier=3),
    # ---- gpueff18 (w8/gpueff18.md): the crowd trim made to work in held scenes (MSC_VGX_CROWDTRIM_WELD, _SKEW, and the older _PROJ for Vice): celebration crowd fragments -36 to -45 % on the host.
    # The B side is scn-on-d / scn-on-v (plan defaults, all three on); run each ge-trim0 run back to back with its scn-on run.
    _r17("ge-trim0-d", "Held moments with the crowd trim as before in held scenes (MSC_VGX_CROWDTRIM_WELD=0, _SKEW=0, _PROJ=0), The Dump", "worstgpu",
         dict(HS, MSC_HITCH_LINES="1500", MSC_VGX_CROWDTRIM_WELD="0", MSC_VGX_CROWDTRIM_SKEW="0", MSC_VGX_CROWDTRIM_PROJ="0"),
         "gpueff18: A side of scn-on-d. Per held class ([head] walk-out, celebration, replay) GPU p50 / p95 and late %: the difference is what the trim buys (ESTIMATE -0.5 to -2 ms GPU in celebrations "
         "and walk-outs, less in replays). Also `[crowdtrim]` lines with MSC_VGX_CROWDTRIM_STAT=1 if added: quads cut in held scenes must be ~0 here and ~all in scn-on-d.", group="s30"),
    _r17("ge-trim0-v", "Held moments with the crowd trim as before in held scenes (MSC_VGX_CROWDTRIM_WELD=0, _SKEW=0, _PROJ=0), Vice tour", "tour",
         dict(HS, MSC_HITCH_LINES="1500", MSC_VGX_CROWDTRIM_WELD="0", MSC_VGX_CROWDTRIM_SKEW="0", MSC_VGX_CROWDTRIM_PROJ="0"),
         "gpueff18: A side of scn-on-v (Vice's held crowd needs _PROJ). Same lines as ge-trim0-d. Look: the crowd in the celebration close-ups (no change expected).", group="s30"),
    _r17("ge-skew0-d", "Held moments with the exact part only (MSC_VGX_CROWDTRIM_SKEW=0), The Dump", "worstgpu",
         dict(HS, MSC_HITCH_LINES="1500", MSC_VGX_CROWDTRIM_SKEW="0"),
         "gpueff18: what the near-exact SKEW cut adds over WELD alone (host: WELD alone cuts about a third of the held crowd quads, SKEW the rest). Only if ge-trim0-d shows a gain.", group="s30", tier=3),
    _r17("scn-mega", "The whole Mega Strike at Vice with the split classes (fast-boot fb-mega-full)", "megafull", dict(HS, MSC_HITCH_LINES="1500"),
         "scenes30: [head] lines of class mega (the Mega scene: 23 Nov 99 % of its pictures at 2 refreshes, GPU 22 ms at 960x544) and mega-cut (the launch and result cut-scenes: 13 % and 26 % at 3 refreshes); "
         "`[head] presentation function \"MegastrikeIntro\" / \"MegastrikeEnd\"` lines prove the split; [heldgov] lines in the mega-cut scenes. Marks @begin / @end mega-sequence.", group="s30"),
    # ---- gpu24 (w8/gpu24.md, 24 Nov): scissor-mask clear clip (vitaGL 0055) and the display scene's mask background. B side = scn-on-d / scn-on-v (plan defaults); run each back to back with its scn-on run.
    _r17("maskclip1-d", "Held moments with the scissor-mask clear over the old region only (MSC_VGX_VGL_MASKCLIP=1), The Dump", "worstgpu",
         dict(HS, MSC_HITCH_LINES="1500", MSC_VGX_VGL_MASKCLIP="1"),
         "gpu24: FIRST check that the pictures are right (a leaking scissor would show in the crowd atlas: shadows or crowd figures cut or smeared in the walk-out and the celebration) and the `[rt] scissor mask clear clip` line "
         "(clears narrowed: about 20 a held picture, kilopixels left out: host model 15,000-24,000 a held picture). Then per held class ([head] walk-out, celebration, replay) GPU p50 / p95 and late % against scn-on-d: the difference is "
         "what the whole-target clears cost (an ESTIMATE only: no console reading of a stencil-only quad exists). Any [fatal], a garbled held scene = stop.", group="s30"),
    _r17("maskclip1-v", "Held moments with the scissor-mask clear over the old region only (MSC_VGX_VGL_MASKCLIP=1), Vice tour", "tour",
         dict(HS, MSC_HITCH_LINES="1500", MSC_VGX_VGL_MASKCLIP="1"),
         "gpu24: as maskclip1-d at Vice; also live play: `[rt] 300 frames: scissor mask rebuilds` is the same count, the GPU in play (`[lane] gpu`) should not rise.", group="s30"),
    _r17("ab-maskclip", "In-run A/B in the replay route: scissor-mask clear clip (MSC_VGX_VGL_MASKCLIP 0 against 1) and the display scene's mask background (MSC_VGX_VGL_MASKBG 1 against 3)", "replay",
         dict(MSC_AB="MSC_VGX_VGL_MASKCLIP,0,1;MSC_VGX_VGL_MASKBG,1,3", MSC_AB_START_S="20", MSC_AB_REPEATS="4", MSC_HEAD_STAT="1"),
         "gpu24: only windows inside a replay or cut-scene show the clip (it acts in the crowd atlas pass); read `n` and expect few usable windows; the single pair maskclip1-d / scn-on-d is the main answer." + AB_WHY, group="s30", tier=3),
    _r17("maskbg1-d", "Held moments with the display scene's mask background off (MSC_VGX_VGL_MASKBG=1: the value before gpu24; plan default 3), The Dump", "worstgpu",
         dict(HS, MSC_HITCH_LINES="1500", MSC_VGX_VGL_MASKBG="1"),
         "gpu24: A side of scn-on-d for the display scene's mask quad (one full-target 960x544 quad a picture). `[rt] scissor mask background` must show scenes begun with the mask set one more a picture in scn-on-d than here; "
         "a garbled or black present = stop (first Vita run of the display bit in this port).", group="s30", tier=3),
]
