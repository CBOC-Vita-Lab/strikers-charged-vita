"""The overnight session (wave 6): one automated night on a real Vita, about 8 hours, ordered by value.

Nothing in 1.3 has ever run on a console. This file is the night's plan as data: phases, runs, the time each phase may use, and for every question the rule that
answers it. gen_kit.py writes env files and plan.json from it (RUNS here, next to matrix.py's), `session.sh night` runs it, issues.py --morning turns the logs into
MORNING.md. The older matrix (matrix.py, tiers 0 to 3, `session.sh core|full|all`) is untouched.

Phases, in the order they run. A phase that has used its time is left (its remaining runs are skipped and listed), so a slow night loses the end, never the start:

  p0  stability      boot, twelve matches in one boot across stadiums (SC01), the eight stadium intros the twelve do not visit, three matches to their natural end
  p1  baselines      1.3 defaults, LOCKED 60 and 1.1 on every scene class (ntour, npipe, nlava, mega, crep), with the 1.1 pictures taken at the same swap
  p2  decisions      each held or default switch against the baseline, in the scene it targets, most valuable first; a drift reference every few runs
  p2z closing        the 1.3 baseline again on every route (same-night noise, drift) and the second LOCKED 60 pair
  p3  pictures       screenshots for the owner's eyes: one set per picture decision, the same moments as the baseline's
  p4  extras         fourth core, boot retry injection, censuses
  p5  second night   what does not fit tonight; the env files exist, `session.sh list` runs them

Measuring protocol (analyse.py --night): a scene stretch is the run of 300-frame records between two scene changes; the first two records of a stretch are the
WARM-UP (the size ladder settling, caches filling) and are dropped when the stretch has a third; the rest are LIVE windows. Per scene: the median of the live windows'
mean, p50, p95 frame time and fps. Baselines repeat (b-* at the start, in the middle as drift references, at the end): same-night noise = 2 x the spread of
the 1.3 defaults of that scene, never below 0.3 fps or 2 % of the baseline. A switch earns its place only when its effect in the scene it targets is beyond the
noise and beyond 0.5 fps, p95 is not worse beyond its noise, late pictures are not worse, nothing crashed, no shader compiled in play, and (for a picture switch) the
screenshots look right.
"""
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
import matrix as M  # noqa: E402

BUDGET_MIN = 480.0          # the night: about 8 hours of console time
DEFAULT_END_MIN = 480.0     # session.sh stops starting runs after this many minutes (NIGHT_MIN overrides)
WARM_RECORDS = 2            # records of a stretch dropped as warm-up (when the stretch has more than that)
WORTH_FPS = 0.5             # the least gain (fps) worth a switch, whatever the noise
NOISE_FLOOR = {"fps": 0.3, "p95": 0.5}
NOISE_REL = 0.02            # noise is never below 2 % of the baseline fps

# the stability and measuring routes of the night
NIGHT_ROUTE = {"tour": "ntour", "pipe": "npipe", "replay": "crep", "mega": "mega", "ctrl": "npipe"}
PERF_ROUTES = ("ntour", "npipe", "nlava", "mega", "crep")
LOCK60 = {"MSC_LOCK60": "1"}
LAVA = dict(M.LAVA)

# phase: (title, minutes it may use counted from the start of the night, i.e. the cumulative deadline; the session skips what is left after it)
PHASES = [
    ("p0", "Stability: boot, twelve matches in one boot, every stadium intro, natural match ends", 75.0),
    ("p1", "Baselines: 1.3 defaults, LOCKED 60 and 1.1 on every scene class, with the 1.1 pictures", 195.0),
    ("p2", "Decisions: each switch against the baseline, in the scene it targets", 385.0),
    ("p2z", "Closing baselines: the same-night noise and drift, the second LOCKED 60 pair", 430.0),
    ("p3", "Pictures: screenshots of each picture decision, for the owner's eyes", 462.0),
    ("p4", "Extras", 478.0),
    ("p5", "Second night: not in tonight's list (env files are ready)", 0.0),
]
PHASE_NAMES = [p[0] for p in PHASES]

# A switch the source of this tree does not read yet (agent branches not merged): the run is dropped from the night's list until it is. {key: branch}
PENDING = {
    "MSC_VGX_PASSDOWN": "agent/passes2", "MSC_FX_COMPOSITE": "agent/megafx", "MSC_VGX_FXCENSUS": "agent/megafx", "MSC_VGX_SBATCH": "agent/merge",
    "MSC_VGX_SBATCH_ORDER": "agent/merge", "MSC_VGX_SBATCH_STATS": "agent/merge", "MSC_CROWD_QUADFAST": "agent/cpu60b", "MSC_POWERUP_HIDDEN": "agent/cpu60b",
    "MSC_SKIN_POSE_NEEDED": "agent/cpu60b", "MSC_VGX_ARENA_LOG": "agent/crash", "MSC_VITA_HEAPPROBE": "agent/crash", "MSC_VGX_ARENA_SOFT": "agent/crash",
}


def _env(entry):
    return dict(entry["env"])


_BY_ID = {r["id"]: r for r in M.RUNS}


def _old(rid):
    return _BY_ID[rid]


RUNS = []
_ids = set()


def add(id, route, env=None, version="1.3", phase="p2", role="nab", title="", why="", group="night", **meta):
    assert id not in _ids, id
    assert phase in PHASE_NAMES, phase
    _ids.add(id)
    r = M.run(id, title or id, route, env, version=version, tier=4, role=role, group=group, why=why, block=phase)
    r["night"] = dict(phase=phase, **meta)
    RUNS.append(r)
    return r


def ab(id, base, route=None, extra=None, phase="p2", on=True, target=None, kind="speed", proof=None, look=None, why="", ref="b", note="", pack=False, **meta):
    """An A/B run: `base` is an old run id of matrix.py (its switches, and by default its route mapped to the night's) or a dict of switches.
    on=True: the run turns the switch ON (is it worth making the default?); on=False: the run turns a default OFF (does it earn its place?).
    ref: the baseline it is judged against: 'b' the 1.3 defaults, 'l' LOCKED 60."""
    if isinstance(base, str):
        old = _old(base)
        env = dict(old["env"])
        route = route or NIGHT_ROUTE.get(old["route"], old["route"])
        title = old["title"]
        why = why or old["why"]
        group = old["group"]
    else:
        env, title, group = dict(base), id, "night"
    env.update(extra or {})
    if route == "nlava":
        env = dict(LAVA, **env)
    return add(id, route, env, phase=phase, title=title, why=why, group=group, on=on, target=target, kind=kind, proof=proof, look=look, ref=ref, note=note, pack=pack, **meta)


def _perf(id, route, version, phase, role, env=None, title="", why=""):
    env = dict(env or {})
    if route == "nlava":
        env = dict(LAVA, **env)
    return add(id, route, env, version=version, phase=phase, role=role, title=title, why=why, group="baseline")


def _looks():
    return {"look": M.SHOTS, "cpipe": M.SHOTS_PIPE, "mega": M.SHOTS_MEGA, "crep": M.SHOTS_REP}


# ---- p0: stability --------------------------------------------------------------------------------------------------------------------------------
BASE = "Baseline: the build's own defaults. Its spread with the other b-* runs of this route is the same-night noise."
# the first run of the night is the shortest scene: it proves the pipeline (launch, log, pull, clocks) before the long stability routes start
add("b-npipe-1", "npipe", phase="p0", role="nref", title="1.3 defaults, npipe (first run: the smoke test, and a baseline)", group="baseline", why=BASE)
add("s-stab", "cstab", phase="p0", role="nstab", version="1.3", title="Stability: twelve matches in one boot across stadiums (3,1,8,15,14,7,9,11,3,1,8,15)", group="stability",
    why="SC01 (pre-match crash, C2-12828-1) was reproduced in Vita3K on 1.1 at the end-of-game cut-scene with 21 MiB of the C heap free: fragmentation plus one large request. "
        "The run must reach script exit with all twelve @end stab-NN marks, no dump, no [arena] FATAL line, and a C heap that does not climb from match to match.", expect=12)
add("s-intro", "cintro", phase="p0", role="nstab", version="1.3", title="Stability: the eight other stadiums, walk-out and a few seconds each", group="stability",
    why="With s-stab every stadium's intro has run once (the intro is the largest frame: 7-12 MiB of vertices).", expect=8)
add("s-end", "cend", phase="p0", role="nstab", version="1.3", title="Stability: three matches played to their natural end (end-of-game cut-scene, results, menu)", group="stability",
    why="The end-of-game cut-scene is where 1.1 crashed in the emulator. Marks end-1..end-3; the third must be followed by script exit.", expect=3)

# ---- p1: baselines --------------------------------------------------------------------------------------------------------------------------------
for rt in PERF_ROUTES:
    if rt != "npipe":
        _perf("b-%s-1" % rt, rt, "1.3", "p1", "nref", title="1.3 defaults, %s (start)" % rt, why=BASE)
for rt in PERF_ROUTES:
    _perf("l-%s-1" % rt, rt, "1.3", "p1", "n60", LOCK60, title="LOCKED 60, %s" % rt,
          why="MSC_LOCK60=1 (Settings > Picture > LOCKED 60) against the 1.3 defaults on the same route: does it hold 60, at what size, with how many late pictures.")
OLD = "1.1 as published (its own source staged as the isolated logging app): the before of before/after."
for rt in PERF_ROUTES:
    _perf("o-%s-1" % rt, rt, "1.1", "p1", "n11", title="1.1, %s" % rt, why=OLD)
for rid, rt, shots in (("o-look", "look", M.SHOTS), ("o-lookpipe", "cpipe", M.SHOTS_PIPE), ("o-lookmega", "mega", M.SHOTS_MEGA), ("o-lookrep", "crep", M.SHOTS_REP)):
    add(rid, rt, dict(shots), version="1.1", phase="p1", role="nlook", title="Look 1.1: %s" % rt, group="look", why="Pictures at a locked 30 fps (never timed), the same moments as the 1.3 rows.")
add("o-stab", "cstab", phase="p1", role="nstab", version="1.1", title="1.1: the twelve-match route (does 1.1 crash where 1.3 does not)", group="stability",
    why="SC01 on the console: 1.1 is expected to fault in a match; 1.3 (s-stab) must not. A crash ends this run early and costs little.", expect=12)

# ---- p2: decisions ---------------------------------------------------------------------------------------------------------------------------------
# In order of value. A run is placed in the scene it acts on: nlava (lava-play: the most draws and fill, items) for renderer and GPU switches, npipe for the mist and
# memory ones, ntour where menus or the walk-out matter, mega and crep for their own cut-scenes. What does not fit the night is phase p5 (second night: env files ready).
DEFAULTS_ALL = _env(_old("defaults-all"))
# 1. the proposed set, against the baseline, on every scene class it can move
for rt, rid in (("ntour", "n-defaults-all"), ("npipe", "n-defaults-all-pipe"), ("mega", "n-defaults-all-mega"), ("crep", "n-defaults-all-rep")):
    ab(rid, DEFAULTS_ALL, route=rt, kind="set", why="Every proposed 1.3 default on together (reports/defaults.md) against the 1.3 baseline: the set ships only if no scene is slower beyond the noise and one is faster.")
# 2. LOCKED 60 (the owner's goal): with the proposed set, and its own knobs
for rt in ("ntour", "npipe", "nlava", "mega"):
    ab("l-defaults-all-%s" % rt, DEFAULTS_ALL, route=rt, extra=LOCK60, kind="set", ref="l",
       why="LOCKED 60 with the proposed set against LOCKED 60 alone: the shipping candidate (the owner's goal: one picture mode that holds 60).")
ab("l-pix45-pipe", {"MSC_LOCK60_PIXSHARE": "0.45"}, route="npipe", extra=LOCK60, ref="l", target=["pipe-play"], why="lock60pix: the model's pixel share of GPU time 0.45 instead of 0.30; against LOCKED 60 on Pipeline Central.")
ab("l-aa0-pipe", {"MSC_LOCK60_AA": "0"}, route="npipe", extra=LOCK60, ref="l", target=["pipe-play"], why="LOCKED 60 without the multisampled rungs above the game's size.")
ab("l-gpubusy0-pipe", {"MSC_GPU_BUSY_FIX": "0"}, route="npipe", extra=LOCK60, ref="l", on=False, target=["pipe-play"],
   why="The GPU time LOCKED 60 steps on, with and without the display scene's wait left out (vitaGL 0030): does the fix hold more 60.")
ab("n-gpubusy0", "gpubusy0", route="npipe", on=False, target=["pipe-play"])
ab("n-between30", "between30", route="npipe", on=False, target=["pipe-play", "pipe-walkout"], why="MSC_FPS_BETWEEN=30 turns the default UP TO 60 pacing off: the 30-only comparison.")
# 3. the players' complaints (SC02 Pipeline Central, SC04 replays and cut-scenes, SC05 Mega Strike)
ab("n-pipemist0", "pipemist0", route="npipe", on=False, target=["pipe-play"])
ab("n-pipeheavy0", "pipeheavy0", route="npipe", on=False, target=["pipe-play", "pipe-walkout"])
ab("n-hazecache3heavy", "hazecache3heavy", route="npipe", target=["pipe-play"], kind="picture")
ab("n-fxruns2", "fxruns2", route="mega", target=["mega-save"], kind="picture")
ab("n-farlod12", "farlod12", route="crep", target=["rep:replay"], kind="picture")
ab("n-shadow2", "shadow2", route="crep", target=["rep:replay", "rep:nis"], kind="picture")
# 4. the proposed defaults one by one, in the scene each acts on
for rid, old, rt, tgt, kind in (("n-fxtight", "fxtight", "npipe", ["pipe-play"], "speed"), ("n-fxgov", "fxgov", "npipe", ["pipe-play"], "speed"),
                                ("n-fxglowhalf", "fxglowhalf", "nlava", ["lava-play"], "picture"), ("n-shadowplay3", "shadowplay3", "nlava", ["lava-play"], "picture"),
                                ("n-shadowplane", "shadowplane", "nlava", ["lava-play"], "speed"), ("n-clusterlazy", "clusterlazy", "crep", None, "speed"),
                                ("n-loadfps60", "loadfps60", "crep", None, "info"), ("n-sweepbg0", "sweepbg0", "nlava", None, "info")):
    ab(rid, old, route=rt, target=tgt, kind=kind)
ab("n-menustill", {"MSC_MENU_STILL": "1", "MSC_PAUSE_STILL": "1"}, route="ntour", target=["menu-main", "stadium-select"], kind="picture", look="n-look-menustill",
   why="Still backdrops in menus and the pause menu: the menu's 99,000 triangles are drawn once, then held (menu-main is triangle-bound, not scene-bound).")
ab("n-viewlayers", {"MSC_VIEW_LAYERS": "1"}, route="nlava", target=["lava-play"], why="Same pixels on the host; lets Prioritize Players act under plan C.")
ab("n-texfull", {"MSC_VGX_LA88": "2", "MSC_VGX_CMPRA": "2", "MSC_VGX_TEX16": "1"}, route="npipe", target=["pipe-play", "pipe-walkout"], kind="picture",
   why="Texture Memory FULL (LA88, CMPRA, 16-bit textures): about 2.9k pixels moved by at most 10 levels on the host. Against the baseline: GPU time, heap, shader compiles.")
ab("n-queuewait", {"MSC_VITA_QUEUE_WAIT": "event"}, route="nlava", target=["lava-play"], why="The frame queue waits on an event instead of polling.")
# Hardware JPEG (galcheck, wave 9; phase p5 because the night is full to the minute): the image now links the four sceJpeg calls (r7 left them null: the default-on codec never opened). One pair on ntour, the route that plays the
# stadium-select preview movies (the intro movie is skipped by the route's A presses): codec on against codec off (MSC_THP_HWJPEG=0, the off switch). The on run must show the census
# line "[thp-hw] sceJpeg: N frames decoded by the codec" with N > 0 (or "codec ready"); a "switched off for this run" line instead means the codec fell back to software (never a crash).
ab("n-hwjpeg1", {"MSC_THP_HWJPEG": "1"}, route="ntour", phase="p5", target=["stadium-select", "menu-main"], proof=r"\[thp-hw\] (codec ready|sceJpeg: [1-9][0-9]* frames decoded by the codec)",
   why="Movie frames decoded by the console's JPEG engine (needs the image that links sceJpeg). Judge: front-end ms in stadium-select, decode-lane load, [thp-hw] opened / decoded / fell back, and the verify lines (mean difference).")
ab("n-hwjpeg0", {"MSC_THP_HWJPEG": "0"}, route="ntour", phase="p5", on=False, target=["stadium-select", "menu-main"],
   why="MSC_THP_HWJPEG=0: the software decoder as in r7. The reference for n-hwjpeg1 on the same night.")
# Free guard (galcheck, wave 9, round 2): vitaGL 0034, every texture / framebuffer delete held 3 presents, a delete of a block a scene drew into reported once per site. Stability run on the route with the
# most target churn (menus, stadium select, walk-out, play, a goal sequence); the proof line is the guard's own start line, and any "FREE WHILE REFERENCED" line in the log is a finding.
ab("x-freeguard3", {"MSC_VGX_FREE_GUARD": "3"}, route="ntour", phase="p5", kind="stability", proof=r"\[free-guard\] on:",
   why="MSC_VGX_FREE_GUARD=3: vitaGL holds every glDeleteTextures / glDeleteFramebuffers 3 presents and reports a delete of a block a scene drew into recently. Judge: no 'FREE WHILE REFERENCED' line, no crash, memory (the held blocks stay 3 presents longer).")
# 5. the switches of waves 5 and 6
ab("n-memfree2", "memfree2", route="npipe", target=["pipe-play"])
ab("n-memfree0", "memfree0", route="npipe", on=False, target=["pipe-play"])
ab("n-maskbg0", "maskbg0", route="nlava", on=False, target=["lava-play"])
ab("n-maskbg1", "maskbg1", route="nlava", target=["lava-play"])
ab("n-maskkeep0", "maskkeep0", route="nlava", on=False, target=["lava-play"])
ab("n-async0", "async0", route="npipe", on=False, target=["pipe-walkout", "pipe-play"], kind="info", why="Asynchronous shader compiles off: a missing shader then stalls the frame (the pack should leave none). Judged by hitches and compiles.")
ab("n-hudnative", "hudnative", route="nlava", target=["lava-play"], kind="picture", look="n-look-hudnative")
ab("n-lodrung0", {"MSC_VGX_LODBIAS_RUNG": "0"}, route="nlava", on=False, target=["lava-play"], kind="picture", look="n-look-lodrung0",
   why="The texture LOD bias that follows the render size (default 60 %): off means the plain mip chain at every size. Cost and picture.")
ab("n-anisomin0", {"MSC_VITA_ANISO_MIN": "0"}, route="nlava", on=False, target=["lava-play"], kind="picture", look="n-look-anisomin0",
   why="Small textures (under 128 texels) keep trilinear by default; 0 sends every texture through anisotropic filtering.")
ab("n-lodsize1", {"MSC_LOD_SIZE": "1"}, route="nlava", target=["lava-play", "lava-walkout"], kind="picture", look="n-look-lodsize1",
   why="Characters take the game's lower mesh by screen size (np/lod.h): only 9 of 21 characters have one.")
ab("n-lodsize2", {"MSC_LOD_SIZE": "2"}, route="nlava", target=["lava-play", "lava-walkout"], kind="picture", look="n-look-lodsize2",
   why="As 1, and the characters without a lower mesh get one built at load by edge collapse (np/lod_simplify.h).")
ab("n-tevcanon2", {"MSC_VGX_TEV_CANON": "2"}, route="nlava", target=["lava-play"], pack=True, kind="speed",
   why="**Shader text changes: pack 15 misses 111 fragment texts** (w6/canon2-pack.md), so the run compiles them: windows holding a compile are dropped, later ones measure the instruction gain (hot shaders 7 to 7: mode 2 alone gains about 1 %).")
ab("n-finclamp2", {"MSC_VGX_TEV_CANON": "2", "MSC_VGX_FINCLAMP": "2"}, route="nlava", target=["lava-play"], pack=True, kind="speed",
   why="**Shader text changes: pack 15 misses all 114 fragment texts.** Compiled forms: -27 % instructions on the hot shaders (8BC3 7 to 5). The question is whether a pack refresh is worth it.")
ab("n-hsr", {"MSC_VGX_HSR_PROVEN": "1", "MSC_VGX_HSR_FACTS": "1", "MSC_VGX_HSR_COUNT": "1"}, route="nlava", target=["lava-play"], proof=r"\[hsrcount\]", kind="info",
   why="No shader text change. [hsrcount] lines count blended draws the key's numbers prove opaque; the frame time says whether turning their blend off pays.")
ab("n-sbatch1", {"MSC_VGX_SBATCH": "1", "MSC_VGX_SBATCH_STATS": "1"}, route="nlava", target=["lava-play"], proof=r"\[sbatch\]", why="Static stadium batches (agent/merge): records saved against front-end and back-end ms.")
ab("n-sbatch2", {"MSC_VGX_SBATCH": "1", "MSC_VGX_SBATCH_ORDER": "1", "MSC_VGX_SBATCH_STATS": "1"}, route="nlava", target=["lava-play"], proof=r"\[sbatch\]")
ab("n-passdown", {"MSC_VGX_PASSDOWN": "0"}, route="crep", target=["rep:replay", "rep:nis"], kind="picture", look="n-lookrep-passdown",
   why="The glow chain's six blur steps drawn at half size straight into their destination: six scenes fewer in replays (agent/passes2).")
ab("n-fxcomp-mega", {"MSC_FX_COMPOSITE": "0", "MSC_FX_STAT": "1"}, route="mega", target=["mega-save"], kind="picture", look="n-lookmega-fxcomp", proof=r"\[fxstat\]",
   why="The effects compositor (agent/megafx): soft-alpha particle layers join the additive ones in one low-resolution target.")
ab("n-fxcomp-pipe", {"MSC_FX_COMPOSITE": "0", "MSC_FX_STAT": "1"}, route="npipe", target=["pipe-play"], kind="picture", look="n-lookpipe-fxcomp", proof=r"\[fxstat\]")
ab("n-quadfast", {"MSC_CROWD_QUADFAST": "1", "MSC_POWERUP_HIDDEN": "1", "MSC_SKIN_POSE_NEEDED": "1"}, route="nlava", target=["lava-play"], why="CPU-side fast paths of agent/cpu60b: crowd quads, hidden power-up slots, skin poses only for named bones.")
# 6. the 1.3 CPU questions (game thread: judged on play and the lane columns) and the picture defaults turned off once
for rid, old, on in (("n-softx128", "softx128", True), ("n-gamefast0", "gamefast0", False), ("n-softx64", "softx64", True), ("n-snaponce0", "snaponce0", False)):
    ab(rid, old, route="ntour", on=on, target=["play"])
for rid, old in (("n-edgeaa0", "edgeaa0"), ("n-aniso0", "aniso0"), ("n-msaa0", "msaa0")):
    ab(rid, old, route="nlava", on=False, kind="picture", target=["lava-play"], look="n-look-" + old)

# ---- p5: the second night (env files ready, not in tonight's list) -----------------------------------------------------------------------------------
for rid, old, on in (("n-gxmdirect1", "gxmdirect1", True), ("n-posecull0", "posecull0", False), ("n-crowdposeonce0", "crowdposeonce0", False), ("n-npclazy0", "npclazy0", False),
                     ("n-crowdrefresh1", "crowdrefresh1", True), ("n-promosolo8", "promosolo8", True), ("n-dltiny64", "dltiny64", True)):
    ab(rid, old, route="ntour", on=on, target=["play"], phase="p5")
ab("n-clockmax", "clockmax", route="ntour", kind="info", target=["play"], phase="p5")
ab("n-amort60ref", "amort60ref", route="ntour", kind="info", phase="p5", why="MSC_FPS=60 alone: the reference of n-amort60 (MSC_AMORT_60 acts only at 60 pictures a second; the LOCKED 60 runs set it themselves).")
ab("n-amort60", "amort60", route="ntour", target=["play"], ref="n-amort60ref", phase="p5")
ab("n-preopen", {"MSC_DVD_PREOPEN": "1"}, route="ntour", kind="info", phase="p5", why="Disc files opened ahead: judged by hitches and load, not by frame time.")
ab("n-lodsize1-rep", {"MSC_LOD_SIZE": "1"}, route="crep", target=["rep:replay", "rep:nis"], kind="picture", phase="p5")
for rid, old in (("n-fg0", "fg0"), ("n-timing0", "timing0"), ("n-unishadow0", "unishadow0"), ("n-pxscissor0", "pxscissor0"), ("n-blind0", "blind0"), ("n-newstadium0", "newstadium0")):
    ab(rid, old, route="ntour", on=False, target=["play"], phase="p5")
ab("n-perf13", "perf13", route="ntour", kind="set", phase="p5", why="PERFORMANCE (60) as the Settings screen writes it, against the defaults.")
ab("n-perf13-pipe", "pipeperf", route="npipe", kind="set", phase="p5")

# drift references: the 1.3 defaults again, between groups of decisions (same-night noise, thermal drift)
for k, rt in enumerate(("npipe", "ntour", "npipe", "ntour"), start=3):
    add("b-%s-d%d" % (rt, k), rt, phase="p2", role="nref", title="1.3 defaults, %s (drift reference)" % rt, group="baseline", why=BASE, drift=True)

# ---- p2z: closing baselines ----------------------------------------------------------------------------------------------------------------------------
for rt in PERF_ROUTES:
    _perf("b-%s-2" % rt, rt, "1.3", "p2z", "nref", title="1.3 defaults, %s (end)" % rt, why=BASE)
for rt in ("ntour", "npipe"):
    _perf("l-%s-2" % rt, rt, "1.3", "p2z", "n60", LOCK60, title="LOCKED 60, %s (end)" % rt, why="The second LOCKED 60 run of the night: its spread with l-%s-1 is the noise of the locked mode." % rt)

# ---- p3: pictures -------------------------------------------------------------------------------------------------------------------------------------
LK = "Pictures at a locked 30 fps (never timed): contact sheet against the 1.3 baseline and 1.1."
# the baseline pictures first (1.3 defaults, LOCKED 60), then one look per picture decision, the owner's most important trade-offs first
for rid, rt, shots, env in (("b-look", "look", M.SHOTS, {}), ("b-lookpipe", "cpipe", M.SHOTS_PIPE, {}), ("b-lookmega", "mega", M.SHOTS_MEGA, {}), ("b-lookrep", "crep", M.SHOTS_REP, {}),
                            ("l-look", "look", M.SHOTS, LOCK60), ("l-lookpipe", "cpipe", M.SHOTS_PIPE, LOCK60), ("l-lookmega", "mega", M.SHOTS_MEGA, LOCK60)):
    add(rid, rt, dict(shots, **env), phase="p3", role="nlook", title="Look %s: %s" % ("LOCKED 60" if env else "1.3 defaults", rt), group="look", why=LK, base=True)
LOOKS = [
    ("n-look-defaults", DEFAULTS_ALL, M.SHOTS, "look"), ("n-lookpipe-defaults", DEFAULTS_ALL, M.SHOTS_PIPE, "cpipe"), ("n-lookmega-defaults", DEFAULTS_ALL, M.SHOTS_MEGA, "mega"),
    ("n-look-edgeaa0", {"MSC_VITA_EDGE_AA": "0"}, M.SHOTS, "look"), ("n-look-msaa0", {"MSC_VGX_MSAA": "0"}, M.SHOTS, "look"), ("n-look-aniso0", {"MSC_VITA_ANISO": "0"}, M.SHOTS, "look"),
    ("n-look-lodsize1", {"MSC_LOD_SIZE": "1"}, M.SHOTS, "look"), ("n-look-lodsize2", {"MSC_LOD_SIZE": "2"}, M.SHOTS, "look"),
    ("n-look-hudnative", {"MSC_VGX_HUDNATIVE": "1"}, M.SHOTS, "look"), ("n-look-lodrung0", {"MSC_VGX_LODBIAS_RUNG": "0"}, M.SHOTS, "look"),
    ("n-look-anisomin0", {"MSC_VITA_ANISO_MIN": "0"}, M.SHOTS, "look"), ("n-look-sharpscale0", {"MSC_VITA_SHARPEN_SCALE": "0"}, M.SHOTS, "look"),
    ("n-look-menustill", {"MSC_MENU_STILL": "1", "MSC_PAUSE_STILL": "1"}, M.SHOTS, "look"),
    ("n-lookmega-fxglowhalf", {"MSC_FX_GLOW": "half"}, M.SHOTS_MEGA, "mega"), ("n-lookmega-fxruns2", {"MSC_FX_LOWRES_RUNS": "2"}, M.SHOTS_MEGA, "mega"),
    ("n-lookmega-fxcomp", {"MSC_FX_COMPOSITE": "0"}, M.SHOTS_MEGA, "mega"), ("n-lookpipe-fxcomp", {"MSC_FX_COMPOSITE": "0"}, M.SHOTS_PIPE, "cpipe"),
    ("n-lookpipe-fxtight", _env(_old("fxtight")), M.SHOTS_PIPE, "cpipe"), ("n-lookpipe-texfull", {"MSC_VGX_LA88": "2", "MSC_VGX_CMPRA": "2", "MSC_VGX_TEX16": "1"}, M.SHOTS_PIPE, "cpipe"),
    ("n-lookpipe-hazecache3heavy", _env(_old("hazecache3heavy")), M.SHOTS_PIPE, "cpipe"),
    ("n-lookrep-passdown", {"MSC_VGX_PASSDOWN": "0"}, M.SHOTS_REP, "crep"), ("n-lookrep-farlod12", {"MSC_REPLAY_FARLOD": "12"}, M.SHOTS_REP, "crep"),
    ("n-lookrep-shadow2", {"MSC_SHADOW_EVERY": "2"}, M.SHOTS_REP, "crep"), ("n-look-shadowplay3", {"MSC_SHADOW_PLAY": "3"}, M.SHOTS, "look"),
    ("n-look-tevcanon2", {"MSC_VGX_TEV_CANON": "2", "MSC_VGX_FINCLAMP": "2"}, M.SHOTS, "look"),
    ("n-look-upfast", {"MSC_VITA_UPSCALE": "fast"}, M.SHOTS, "look"), ("n-look-upcombined", {"MSC_VITA_UPSCALE": "combined"}, M.SHOTS, "look"), ("n-look-upcas", {"MSC_VITA_UPSCALE": "cas"}, M.SHOTS, "look"),
]
for rid, extra, shots, rt in LOOKS:
    add(rid, rt, dict(shots, **extra), phase="p3", role="nlook", title="Look: " + rid[2:], group="look", why=LK)
# the look that shows each picture decision's change
LOOK_OF = {"n-fxglowhalf": "n-lookmega-fxglowhalf", "n-fxruns2": "n-lookmega-fxruns2", "n-fxtight": "n-lookpipe-fxtight", "n-texfull": "n-lookpipe-texfull",
           "n-hazecache3heavy": "n-lookpipe-hazecache3heavy", "n-farlod12": "n-lookrep-farlod12", "n-shadow2": "n-lookrep-shadow2", "n-shadowplay3": "n-look-shadowplay3",
           "n-defaults-all": "n-look-defaults", "n-defaults-all-pipe": "n-lookpipe-defaults", "n-defaults-all-mega": "n-lookmega-defaults", "n-tevcanon2": "n-look-tevcanon2", "n-finclamp2": "n-look-tevcanon2"}
for r in RUNS:
    if r["id"] in LOOK_OF:
        r["night"]["look"] = LOOK_OF[r["id"]]

# ---- p4: extras ---------------------------------------------------------------------------------------------------------------------------------------
ab("x-core3-pipe", {"MSC_VITA_CORE3": "1", "MSC_VITA_CORE3_SEND": "1"}, route="npipe", phase="p4", target=["pipe-play"], proof=r"fourth core|\[vgx-vita\] the fourth core",
   why="With CapUnlocker the log says `[vita] fourth core: usable`; without it `not available` and the run equals b-pipe-*. Compare the [lane] game column.")
ab("x-core3-tour", {"MSC_VITA_CORE3": "1", "MSC_VITA_CORE3_SEND": "1"}, route="ntour", phase="p4", target=["play"], proof=r"fourth core")
ab("x-bootio", {"MSC_BOOTIO_INJECT": "2"}, route="npipe", phase="p4", kind="stability", proof=r"\[bootio\]",
   why="traps: the first two start-up file opens fail with EIO; the retry (50 ms apart, up to 3 s) must get the game up. Pass: [bootio] lines, then the route ends. This is the fopen-race fix, on a real Vita's libc.")
ab("x-hsrcensus", {"MSC_VGX_HSR_COUNT": "1", "MSC_STADIUM_CENSUS": "1"}, route="npipe", phase="p4", kind="info", proof=r"\[hsrcount\]")
ab("x-fxcensus", {"MSC_VGX_FXCENSUS": "600"}, route="mega", phase="p4", kind="info", proof=r"\[fxcensus\]")
ab("x-arena", {"MSC_VGX_ARENA_LOG": "1", "MSC_VITA_HEAPPROBE": "1"}, route="cstab", phase="p5", kind="stability", proof=r"\[arena\]",
   why="The stability route again with the arena and heap probe lines (agent/crash): peaks of the frame buffers and the largest free C-heap block per match.", expect=12)
ab("l-lock60-lava-pix45", {"MSC_LOCK60_PIXSHARE": "0.45"}, route="nlava", extra=LOCK60, phase="p4", ref="l", target=["lava-play"])


# ---- plan helpers --------------------------------------------------------------------------------------------------------------------------------------
_KEYS = None


def source_keys(repo=REPO):
    """The MSC_/NP_ names this tree's source reads (grep over the code), so a run whose switch no source reads can be left out of the list."""
    global _KEYS
    if _KEYS is not None and repo == REPO:
        return _KEYS
    dirs = [str(Path(repo) / d) for d in ("native", "renderer", "dependencies/vitaGL/source", "src") if (Path(repo) / d).is_dir()]
    keys = set()
    if dirs:
        out = subprocess.run(["grep", "-rIhoE", '"(MSC|NP)_[A-Z0-9_]+"', "--include=*.cpp", "--include=*.c", "--include=*.h", "--include=*.hpp"] + dirs,
                             capture_output=True, text=True).stdout
        keys = {m.strip('"') for m in out.split()}
    if repo == REPO:
        _KEYS = keys
    return keys


def missing_keys(run, keys):
    """The switches of a run that no source reads (with their branch from PENDING when it is known)."""
    return sorted({k for k, _ in run["env"] if k.startswith(("MSC_", "NP_")) and k not in keys})


def pending_for(run, keys):
    return [(k, PENDING.get(k, "?")) for k in missing_keys(run, keys)]


def minutes(r):
    return M.minutes(r)


def phase_of(r):
    return r["night"]["phase"] if "night" in r else None


def order(keys=None, with_pending=False):
    """The night as ('swap', version) and ('run', entry) steps and the list of dropped runs [(id, [(key, branch)])].
    Order: phase order, then the order of RUNS inside a phase, except that the 1.1 runs of p1 come together (one swap there and one back). Swaps are made lazily by the
    session (only when a run that needs the version is about to start), the swap lines are here for the estimate."""
    keys = source_keys() if keys is None else keys
    steps, dropped, current = [], [], "1.3"
    for ph in PHASE_NAMES:
        if ph == "p5":
            continue
        runs = [r for r in RUNS if phase_of(r) == ph]
        # inside p1: 1.3 runs first, then the 1.1 block (looks and stability last), so the console swaps twice
        runs.sort(key=lambda r: (r["version"] != "1.3",))
        ok = []
        for r in runs:
            miss = [] if with_pending else pending_for(r, keys)
            if miss:
                dropped.append((r["id"], miss))
            else:
                ok.append(r)
        for r in ok:
            if r["version"] != current:
                steps.append(("swap", r["version"]))
                current = r["version"]
            steps.append(("run", r))
        # a phase ends on the 1.3 build again so the next phase starts without a swap
        if current != "1.3" and ph == "p1":
            steps.append(("swap", "1.3"))
            current = "1.3"
    return steps, dropped


def interleave_drift(steps):
    """Spread the drift references (b-*-dN) of p2 through its decisions: one after every ninth decision run."""
    drift = [s for s in steps if s[0] == "run" and s[1]["night"].get("drift")]
    rest = [s for s in steps if not (s[0] == "run" and s[1]["night"].get("drift"))]
    out, n, di = [], 0, 0
    for s in rest:
        out.append(s)
        if s[0] == "run" and phase_of(s[1]) == "p2":
            n += 1
            if n % 9 == 0 and di < len(drift):
                out.append(drift[di])
                di += 1
    # the last phase-2 decision is followed by the rest of the drift references, if any are left
    for d in drift[di:]:
        idx = max(i for i, s in enumerate(out) if s[0] == "run" and phase_of(s[1]) == "p2")
        out.insert(idx + 1, d)
    return out


def schedule(keys=None, with_pending=False):
    steps, dropped = order(keys, with_pending)
    return interleave_drift(steps), dropped


def estimate(keys=None, measured=None, with_pending=False):
    """(total minutes, rows [(label, phase, minutes, cumulative)], dropped)."""
    measured = measured or {}
    steps, dropped = schedule(keys, with_pending)
    rows, cum = [("setup", "p0", M.SETUP_MIN, M.SETUP_MIN)], M.SETUP_MIN
    for kind, item in steps:
        if kind == "swap":
            m, label, ph = M.SWAP_MIN, "swap to " + item, "p1"
        else:
            m, label, ph = measured.get(item["id"], minutes(item)), item["id"], phase_of(item)
        cum += m
        rows.append((label, ph, m, cum))
    return cum, rows, dropped


def phase_ends(rows):
    """{phase: cumulative estimate at the end of its last row}"""
    out = {}
    for label, ph, m, cum in rows:
        out[ph] = cum
    return out


def meta(r):
    """The run's night entry for plan.json: phase, kind, target scene classes, proof pattern, the look run, which baseline it is judged against."""
    n = dict(r.get("night", {}))
    n.setdefault("kind", {"nref": "baseline", "n60": "baseline", "n11": "baseline", "nlook": "look", "nstab": "stability"}.get(r["role"], "speed"))
    return n
