"""The r17 consolidated console sitting (kit20, 15 Nov): `session.sh list r17-sitting.txt` is the sitting (blocks A and B, ends at the natural stop with the owner's settings restored),
`session.sh list r17-sitting-extra.txt` is everything else in order of value (units X1... kept whole, each ends with the plain executable back). Both are generated:

    python3 tools/console-1.2/gen_kit.py            # writes r17-sitting.txt, r17-sitting-extra.txt, r17-lan.txt (and --check says when they are out of date)

The owner's goal decides the order: close to 60 fps at full resolution everywhere, the rest a smooth 30, no stutter at load, walk-outs, goals, replays or videos, no pop-in. The 60 mode
("Smooth", MSC_LOCK60) is enabled by default only if the console proves it, so the 60 answer comes first.
  A  (about 2 h)  the plain r17 executable: references d1 / worstgpu1; the 60 answer (lock60tour, lock60pipe, lock60dump, lock60lava with MSC_HEAD_STAT=1; lock60gs-v / -d with the split lines);
     the match step on its own thread (simt-blk-v first: the gate; simt-v, simt-d); then the owner's stutter fixes of r16 block A (crep1, discard pair, memory and load runs, scene pin, held scenes,
     cut learning) and the goal-scenario slot (route16)
  B  the PGO answer: plain r17 against r17pgo, alternating, at Vice, The Dump and at 60 (pgo-*-lock60), and simt-pgo; ends with the plain executable back, the owner's settings restored, NATURAL STOP
     (A + B at most 3 h 15)
  X1...  the extra list, in order of value (CONSOLE-PLAN-r17.md has the table with cumulative minutes)

Fast boot: each list starts with `fastboot 1`; session.sh then uses env-fb/<id>.txt and the fast-boot route of every run that has one (matrix.fastboot_route). The minutes below are ESTIMATES
(matrix.py ROUTE_MIN minus FB_SAVE_MIN, plus RUN_OVERHEAD_MIN a run for the env upload, quit and pull); the session writes the real times (session-times.csv).
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import matrix as M  # noqa: E402

RUN_OVERHEAD_MIN = 0.4      # env upload and read-back, quit, nap, log pull on top of the route's own minutes
ESWAP_MIN = 1.0             # eboot.bin alone (about 10 MB) pushed and read back, no shader pack
SETUP_MIN = M.SETUP_MIN
PKG_BASE, PKG_PGO = "r17", "r17pgo"     # versions/<name>/eboot.bin (prepare-versions.py --eboot r17=<vpk> --eboot r17pgo=<vpk> ...): the final r17 packages
PKG_PGOX, PKG_PGOXM = "r17pgox", "r17pgoxm"   # link17: s17-pgo-x (hot functions in profile order) and s17-pgo-xm (+ NP_MTX_LTO=1), built from the final tree
VER_PACK19, VER_BASE = "pack19", "1.3"        # `swap pack19` = eboot.bin + shader pack19 (prepare-versions.py --vpk pack19=<vpk>); `swap 1.3` puts the r17 package's own pack back (--vpk 1.3=<r17 vpk>)

# Slots: runs that are still being built. A slot with ids turns into its run lines by itself when every id exists in matrix.py (the branch merged and gen_kit.py run again); `manual` False keeps
# a slot closed (the coordinator sends the lines, kit20 adds the runs and sets manual None). A closed slot prints its marker and reserves its minutes.
SLOTS = [
    dict(name="route16", branch="agent/route16", ids=["gsvice16", "gsdump16", "gslava16"], manual=True, reserve=12.0,
         what="goal scenarios with the test-only `score` command (a goal, celebration and replay in the first minute) at Vice, The Dump and Lava Pit, split lines and the held-scene hitch budget on (merged; the Lava goal path is clean in the emulator)"),
    dict(name="dumpgpu10", branch="agent/dumpgpu10", ids=["dumpcut1", "dumpcut1l", "dumprain1"], manual=None, reserve=15.0,
         what="the stadium cuts (MSC_STADIUM_CUT, visible, owner's eye on the sheets): Lava Pit heat shimmer off (dumpcut1l), The Dump crowd floor (dumpcut1), The Dump rain streaks 1 in 2 (dumprain1)"),
]
OPEN_SLOTS = [   # helpers still running: lines to be sent by the coordinator, then added to matrix.py and to the unit named in `unit`
    dict(name="pace18", branch="agent/pace18", ids=[], manual=False, reserve=10.0, unit="X2", what="the sim thread core map (MSC_VITA_CORE_SIM, priorities, np_fps cost model with join wait): runs against simt-v"),
    dict(name="shadowst19", branch="agent/shadowst19", ids=[], manual=False, reserve=10.0, unit="X3", what="stadium shadows (the shadow quad of The Dump / Lava)"),
    dict(name="alias19", branch="agent/alias19", ids=[], manual=False, reserve=10.0, unit="X4", what="depth sharing across same-size scenes (alias15 successor)"),
    dict(name="simfix20", branch="agent/simfix20", ids=[], manual=False, reserve=0.0, unit="X3", what="the lost wake-up fix package for the simt runs (the lines follow with the merge; until then every simt run is marked as needing it)"),
    dict(name="front19", branch="agent/front19", ids=[], manual=False, reserve=5.0, unit="X5", what="the OPTIONS corner button VITA SETTINGS (front-end popups; route-driven where possible)"),
]


def _ids():
    return {r["id"] for r in M.RUNS}


def slot_live(s):
    if s["manual"] is not None:
        return bool(s["manual"]) and bool(s["ids"]) and all(i in _ids() for i in s["ids"])
    return bool(s["ids"]) and all(i in _ids() for i in s["ids"])


def _abba(on, off, n="abc"):
    """Alternating order cancelling drift: on, off, off, on, on, off ..."""
    out = []
    for k, c in enumerate(n):
        out += [on % c, off % c] if k % 2 == 0 else [off % c, on % c]
    return out


R = lambda *ids: [("run", i) for i in ids]   # noqa: E731
C_DUMP_START = ("dp16a-%s", "dp16b-%s")
C_R15_CUTS = ["cutsshapes1", "cutsshapes1-2", "warm1", "warm1-2", "dump-all", "dump-all-2", "renderrng1", "bg30pair", "bg30pairdump", "bg30dump", "bg30half", "circ24", "la2-on"]
DUMPGPU20 = ["dg20-s2-size-d960", "dg20-s2-size-d912", "dg20-s2-size-d864", "dg20-s3-size-v960", "dg20-s3-size-v912", "dg20-s3-size-v864",
             "dg20-s4-flat-d", "dg20-s5-pixel-d", "dg20-s6-blend-d", "rainoff-d", "dg20-s7-aniso0-d", "dg20-s8-lod0-d", "dg20-s9-pb8-d", "dg20-s11-passes-d", "dg20-s12-rain2-d", "dg20-s14-up-d", "dg20-s13-skin0-d"]


def _open(name):
    s = next(x for x in OPEN_SLOTS if x["name"] == name)
    return ("slot", s)


def steps():
    """The sitting (r17-sitting.txt) as (kind, payload): head / note (comment lines), run (id), eswap (name), restore, fastboot, slot (dict), stop."""
    st = [("fastboot", 1)]
    st.append(("head", "A: the plain r17 executable at its defaults: the 60 answer first, then the match step on its own thread, then the stutter fixes (eboot.bin of CDXMP0001 swapped to r17 first; nothing else is installed: the shader pack is unchanged). Every run here also has MSC_HITCH_LINES=1500"))
    st.append(("eswap", PKG_BASE))
    st.append(("note", "references: Vice tour and The Dump (the walk-out, play and goal at both; the tour's menu stretch is not comparable with older logs)"))
    st.append(("note", "dlweld0 right after worstgpu1 (fable-da21 F7): the vertex weld is ON in every run of the sitting (plan default); this is its OFF side. Pass rule: `[dlweld]` present in worstgpu1, no new [hitch] at the walk-out / goal cuts against dlweld0, no fault; keep the weld on unless the on side is slower or faults"))
    st += R("d1", "worstgpu1", "dlweld0")
    st.append(("note", "THE 60 ANSWER (owner: 60 where the console proves it): LOCKED 60 with MSC_HEAD_STAT=1 at Vice (menus, play, Mega Strike, replay), Pipeline Central, The Dump and Lava Pit. Pass rule: p50 16.7, late under 2 %, no relief in open play, [head] game-thread p95 under 14.5 ms, [split] held == finished, leaked 0. A stadium class that relieves stays Detailed-only"))
    st += R("lock60tour", "lock60pipe", "lock60dump", "lock60lava")
    st.append(("note", "heat shimmer cuts are ON by default (never on a console): warboff-l is the OFF side of lock60lava; `[acts]` (EFBSWAP_NEXT swaps, CLEARVEH copies) must be above 0 on the on side (read gslava16 at picture 4200 if lock60lava ends before its first play line)"))
    st += R("warboff-l")
    st.append(("note", "the screens every 1.3 player gets, driven by the route: the in-match settings leave dialog (a next-start row: SAVE / DON'T SAVE / KEEP EDITING, no restart) and the pause menu's VITA SETTINGS screen (scene 88)"))
    st += R("live19-leave", "scene18-pause")
    st.append(("note", "head17 + split lines at the same stadiums (the game-thread p95 with and without a step, the render-lane and GPU p95: the reference of the simt runs)"))
    st += R("lock60gs-v", "lock60gs-d")
    st.append(("note", "the match step on its own thread (needs the simfix20 package, MSC_OVERLAP_LOG=1 in every run): simt-blk-v FIRST and STOP all simt runs on any fault or a counter not 0; then the overlapped step at Vice against the reference above (simt-d, The Dump, is the first run of X3). MSC_SIMT never enters the Smooth table from this sitting"))
    st += R("simt-blk-v", "simt-v")
    st.append(("note", "the owner's stutter at the stadium load, walk-outs, goals and replays: goalie close-up replay with the texture census; the discard pair (two a side, stall10: MSC_DISCARD_FAST); The Dump first for the memory runs (nodepth1: any fault or corrupted picture and it is never used); hidden-frame rehearsal and load hold; scene pin (run twice: the second acts)"))
    st += R("crep1")
    st += [("run", i) for i in _abba("dis-on-%s", "dis-off-%s", "ab")]
    st += R("ram112-dump", "nodepth1", "ram112-a", "rehearse6", "loadhold1", "scenepin1", "scenepin1-2")
    st.append(("note", "Vice held scenes (held16): one sample in replays and cut-scenes (the owner's choice) with the paced warm (run twice: the second acts); cut learning of programs and variants (learn16, run twice)"))
    st += R("heldaa13w2", "heldaa13w2-2", "cutlearn2", "cutlearn2-2")
    for s in SLOTS:
        if s["name"] == "route16":
            st.append(("slot", s))
    st.append(("head", "B: the PGO answer (eboot.bin only, session.sh eswap): plain r17 against r17 PGO, alternating, at Vice, The Dump and at the 60 mode; WATCH THE FIRST 60 s OF THE FIRST PGO RUN FOR A CRASH"))
    st.append(("note", "the plain d1 / lock60 runs here are also repeats of the block A references (their noise); the PGO d1 / worstgpu pairs are the old judgement, pgo-*-lock60 the 60 mode's game-thread p95, simt-pgo the executable that would ship with the overlapped step"))
    seq = [("r", "pgo-base-d1-a"), ("r", "pgo-base-wg-a"), ("r", "pgo-base-lock60"),
           ("e", PKG_PGO), ("r", "pgo-on-wg-a"), ("r", "pgo-on-d1-a"), ("r", "pgo-on-lock60"), ("r", "simt-pgo"),
           ("e", PKG_BASE), ("r", "pgo-base-wg-b"), ("r", "pgo-base-d1-b"),
           ("e", PKG_PGO), ("r", "pgo-on-d1-b"), ("r", "pgo-on-wg-b"),
           ("e", PKG_BASE)]
    st += [("eswap", x) if k == "e" else ("run", x) for k, x in seq]
    st.append(("restore", ""))
    st.append(("stop", "NATURAL STOP POINT: blocks A and B are finished, the r17 executable is back on the console and the owner's np-player.txt is restored. Everything else: r17-sitting-extra.txt, in order"))
    return st


def _extra_units():
    """The extra list as units in order of value (a unit is kept whole). Each: (code, [steps])."""
    U = []
    U.append(("ON", [("head", "ON: the REAL online service (msc-online: strikers.psvita.dev first, the old address as the fallback), one console through the game's own menus, no opponent (online24; about 15 min; needs the executable with MSC_NET_REQLOG, i.e. a build that has agent/online24: without its `[np-net] req` lines the pass rules cannot be read and online_check.py says INVALID). Order: sign-in (new address first, then the old only), the two read-only lists, the quick and ranked queues (60 s wait, cancel), the report. The connection check (on-conn) is BY HAND: its only entry is the failure popup after a failed HOST A ROOM, so it needs a Wi-Fi with no internet (no np-player.txt line can fail the first try: the isolated reader refuses MSC_ONLINE_URL). Every run sets MSC_ONLINE_HOST=auto (on-hub-old: old) and MSC_ONLINE_NAME (a preset name: nothing is typed). The FIRST sign-in of the sitting makes the test account on the live service; the second on-hub run proves the stored token. Pass rules in the plan and in each run's why text; python3 tools/console-1.2/online_check.py <log> --run <id> applies them")]
              + R("on-hub", "on-hub-old", "on-lists", "on-quick", "on-ranked", "on-report")))
    U.append(("MP", [("head", "MP: multiplayer on ONE Vita (mp18, owner 23 Nov 'make sure multiplayer works'; about 13 min; today's plan, the fourth core on: needs CapUnlocker for the guard lines). Same Wi-Fi host alone (UDP 25100, the lobby, the core-3 guard off and back), NEARBY host alone (the ad hoc radio; OK the system's Wi-Fi dialog by hand if it shows), online sign-in, HOST A ROOM alone (UDP 25110, the room on the live service, 45 s, cancel), the same RELAY ONLY (the TURN relay allocated = TURN reachable), the quick queue 60 s. A match needs a second device: w8/mp18.md. python3 tools/console-1.2/online_check.py <log> --run <id> applies the pass rules")]
              + R("mp-lan-wifi", "mp-lan-adhoc", "mp-hub", "mp-host", "mp-host-relay", "mp-quick")))
    U.append(("X1", [("head", "X1: the section-ordered PGO executable (link17; needs versions/r17pgox and r17pgoxm). Against the pgo-on runs of block B; ship the order-only build unless the xm build gains more. Ends with the plain executable back")]
              + [("eswap", PKG_PGOX)] + R("pgox-d1-a", "pgox-wg-a", "pgox-lock60", "pgox-d1-b", "pgox-wg-b")
              + [("eswap", PKG_PGOXM)] + R("pgoxm-d1") + [("eswap", PKG_BASE)]))
    U.append(("X2", [("head", "X2: the settings screens that ship, driven by the route's own pad and finger presses (live19, scene18): the at-once rows (the leave dialog and the pause screen are in block A). The route cannot press the back panel or judge the OLED: see the by-hand list in CONSOLE-PLAN-r17.md")]
              + R("live19-shake", "live19-crowd", "live19-rear") + [_open("front19")]))
    U.append(("X3", [("head", "X3: more of the overlapped step (needs the simfix20 package; only if simt-blk-v was clean and simt-v / simt-d ran): Lava Pit, the held 30 at The Dump, press-to-picture, then where np_sim belongs on a stock console. simt-c3 only if an earlier log's core-3 probe line says usable")]
              + R("simt-d", "simt-lava", "simt30-d", "simt-lat", "simt-c1", "simt-c12", "simt-fs12", "simt-c3") + [_open("pace18"), _open("simfix20")]))
    U.append(("X4", [("head", "X4: the Dump's GPU time (dumpgpu20 S2-S14 and gpuscene20, order = value; every run pins 704x400 one sample unless it names a size): can The Dump reach 60 at all, and which part of its cost is fixed work, shading, filtering, blends")]
              + [("note", "S2 / S3 size slopes at The Dump and Vice (decide fixed against per-pixel excess); S4 flat / S5 pixel / S6 blend hunts, rainoff-d after S6; S7 aniso / S8 mip bias; S9 parameter buffer 8 MiB (S10, the 32 MiB one, only if S9 is worse: run it by hand: session.sh run dg20-s10-pb32-d); S11 passes; S12 rain; S14 final pass; S13 skin. S1 (tiling split) is covered by scenes-d / scenes-v")]
              + R(*DUMPGPU20)
              + [("note", "swz20 (swizzled textures; the swizzle stays OFF in the plan): `swz-d` then `swz-v` ([lane] gpu against worstgpu1 / d1, every HUD icon right, `[texclass]` counts; any wrong icon = stop the swz runs), `swz-m` (mip chains) only if `swz-d` was clean. MSC_VGX_TEXSWIZZLE=1 only after a clean console round shows lower GPU ms at The Dump and no wrong icon.")]
              + R("swz-d", "swz-v", "swz-m")
              + [("note", "LAST: the gpuscene20 diagnostics (per-scene GPU ms, parameter-buffer estimate, texture layout). The emulator showed 47 guest faults with all four on (none with them off): a fault in scenes-d / scenes-v is a diagnostic failure, not a game failure")]
              + R("scenes-d", "scenes-v") + [_open("shadowst19")]))
    U.append(("X5", [("head", "X5: the GPU levers at 60 for The Dump and Lava Pit (exact switches never run on a console, with their verify modes; every verify count must be 0), then LOCKED 60 with them on (the weld is a plan default now: its OFF side dlweld0 is in block A)")]
              + R("exactall-vice", "exactall-dump", "lock60x-dump", "lock60x-lava")))
    U.append(("X6", [("head", "X6: video memory (the 24 MiB free-memory line at kick-off and at goal cuts): render-target pool and no-depth passes (nodepth1 was judged in block A: any fault and nodepth1v is skipped and the switch never used), who holds the memory (memsite17), the patcher buffer at 2 MiB and the parameter buffer in LPDDR (vram19; a hang in pbl1 = fail), then the helpers' slots")]
              + R("nodepth1v", "rtpool1", "megartpool1", "memsite-d", "memsite-v", "patcherbuf", "pbl1") + [_open("alias19")]))
    U.append(("X7", [("head", "X7: game-thread probes (cheap): MSC_AI_NOCLOCK=1 is ON in the plan since r17 (exact on the host); price17a / probe18b are its OFF side (clock read as before) and price17b / probe18a the FPS probe. Keep NOCLOCK on unless price17a shows no more ms, or a new [hitch] / changed [fps] decisions appear")]
              + R("price17ref", "price17a", "price17b", "probe18a", "probe18b", "cpu22v-v", "cpu22v-d", "cpu22on-v")))
    U.append(("X8", [("head", "X8: heat shimmer at Lava Pit and Sand Tomb (warble19, ON in the plan): warboff-l (the Lava OFF side) is in block A; here the exact cuts with LITE 0 and the Sand Tomb off side, and the 30 Hz map (Lava: lock60lava of block A IS that run, LITE 1 is the plan default; Sand Tomb LITE 3).  One round: a difference under the noise of d1-type repeats is no result")]
              + R("warb-l", "warboff-s", "warb-s", "warbl-s")))
    U.append(("X9", [("head", "X9: stutter candidates of r16 that were not in block A: the held-scene policy, scene counts, cadence, the broad-phase verify, press-to-picture, pop-in, the r15 warm cap, back-pressure")]
              + [("note", "held16: the old warm (only if heldaa13w2 booted); r15 held-scene side heldaa13; the scene pin and policy (scenepol1 first console run of the policy; scenepol1s adds size sets); scene count (scenes16)")]
              + R("heldaa13w1", "heldaa13", "scenepol1", "scenepol1s", "heldscn3", "heldscn7")
              + [("note", "the steady 3-refresh cadence in held scenes (cadence16); gt16b: broad phase verify on the ARM path (wrong=0); press-to-picture (press16: baseline, sampling cut, true-room latch, the cut at The Dump); materials at the walk-out (popin16)")]
              + R("cad3", "cad3d", "bpverify", "lat16a", "lat16b", "lat16c", "lat16d", "popin1")
              + [("note", "Galaxy ports: back-pressure gate without the pad sleep (lessons17), the r15 warm-up cap 300 against r16's 400 (comp17)")] + R("l17d", "c17base")))
    U.append(("X10", [("head", "X10: the overclock A/B (clock17: every other run asks 500 one second after start; these hold 444; read the `clocks: policy` line and the later `clocks: the ARM core measures` 30 s line, NOT `[clock] boot:`; the PSVshellPlus profile of the test title must not lock 500 during these two) and the game-thread off sides")]
              + R("clock444", "clock444d")
              + [("note", "game thread (gt16a, gt16b): the off sides of the exact cuts that are ON, the verify of the AI loop, the one-copy and NEON switches that are OFF; MSC_GSPLIT=2 in each")]
              + R("bpfull0", "aiavoid0", "snap0", "rneon1", "aiavoid7", "rneonv", "bpfull1", "smooth0")))
    U.append(("X11", [("head", "X11: the whole kit's other new switches: the freeze button idle (the by-hand press is in the hands list), tight particle polygons with the compositor off (census19: invisible there, saves fill in cut-scenes), The Dump start step (three a side, dumpplay16)")]
              + R("freezeidle", "fxc0-pipe", "fxtight-c0-pipe", "fxc0-lava", "fxtight-c0-lava")
              + [("run", i) for i in _abba(C_DUMP_START[0], C_DUMP_START[1])]))
    U.append(("X12", [("head", "X12: the shader pack candidate (list17): pack19 with the warm-up cap 500 against d1 (swap pack19, run, swap 1.3 puts the r17 package's own pack back; needs versions/pack19 and versions/1.3)")]
              + [("swap", VER_PACK19)] + R("l17pack19") + [("swap", VER_BASE)]))
    U.append(("X13", [("head", "X13: cut learning of programs (learn16: each twice, the second launch acts), the A/B of four r15 defaults, the stadium-cut slot")]
              + R("cutlearn1", "cutlearn1-2", "cutlearn3d", "cutlearn3d-2", "megad1", "megasharp0", "megaballs0", "megasharp2", "crowdgtoff") + [("slot", SLOTS[1])]))
    U.append(("X14", [("head", "X14: the r15 cuts switches (each twice), dump-all, the render streams, background-at-30 runs, memory runs, lessons17's bg30 pair and press margin")]
              + R(*C_R15_CUTS) + R("lat16e", "l17a", "l17b", "l17c")))
    U.append(("X15", [("head", "X15: pacing (pace10), round by round: today's AUTO, even 30, even 30 + phase sync, + hold budget margin 0.90; three rounds at Vice and at The Dump (the model expects about no gain: last)")]
              + [("run", "%s%d" % (i, n)) for n in (1, 2, 3) for i in ("v1c", "v2l", "v4p", "v5h")]
              + [("run", "%s%d" % (i, n)) for n in (1, 2, 3) for i in ("d1c", "d2l", "d4p", "d5h")]))
    return U


def extra_steps():
    st = [("fastboot", 1), ("head", "X: the extra list, in order of value. Units X1... are whole (a unit that ends the evening ends with the plain executable on the console); `restore` at the end puts the owner's np-player.txt back. The cumulative minutes are in CONSOLE-PLAN-r17.md")]
    for code, u in _extra_units():
        st += u
    st.append(("restore", ""))
    st.append(("stop", "END OF THE EXTRA LIST"))
    return st


def _by_id():
    return {r["id"]: r for r in M.RUNS}


def _line(r):
    return "run\t%s\t%s\t%s\t%s" % (r["id"], M.fastboot_route(r) or r["route"], round(M.minutes(r, True), 2), r["version"])


def sitting_text(extra=False):
    by = _by_id()
    name = "r17-sitting-extra.txt" if extra else "r17-sitting.txt"
    out = ["# r17 consolidated sitting%s (generated by gen_kit.py from sitting.py; edit that, not this file)." % (" (extra list, in order of value)" if extra else ""),
           "# bash session.sh list %s    (finished runs are skipped: the same command resumes a cut sitting)" % name,
           "# Lines: fastboot / swap / eswap / restore / run (tab separated); every other line is a comment."]
    for kind, p in (extra_steps() if extra else steps()):
        if kind == "fastboot":
            out.append("fastboot\t%d" % p)
        elif kind == "head":
            out.append("# ==== %s" % p)
        elif kind in ("note", "stop"):
            out.append("# ---- %s" % p)
        elif kind == "eswap":
            out.append("eswap\t%s" % p)
        elif kind == "swap":
            out.append("swap\t%s" % p)
        elif kind == "restore":
            out.append("restore")
        elif kind == "run":
            out.append(_line(by[p]))
        elif kind == "slot":
            if slot_live(p):
                out.append("# ---- slot %s (%s): %s" % (p["name"], p["branch"], p["what"]))
                out += [_line(by[i]) for i in p["ids"]]
            else:
                out.append("# ---- SLOT %s (%s, still being built; about %.0f min reserved): %s." % (p["name"], p["branch"], p["reserve"], p["what"]))
                out.append("# ---- Its lines join this list when the coordinator sends them and gen_kit.py is run again (kit20 / sitting.py OPEN_SLOTS)." if not p["ids"] else
                           "# ---- It joins this list by itself when %s %s in matrix.py and gen_kit.py is run again." % ("/".join(p["ids"]), "exist" if p["manual"] is None else "are made ready (sitting.py SLOTS manual=True) and exist"))
    return "\n".join(out) + "\n"


def _block_of(head):
    code = head.split(":")[0].strip()
    return code if len(code) <= 3 else head.split()[0]


def rows(extra=False):
    """(label, minutes, block) for the table; slots that are not live count their reserve."""
    by = _by_id()
    block, res = "A", [("setup", SETUP_MIN, "A")]
    for kind, p in (extra_steps() if extra else steps()):
        if kind == "head":
            block = _block_of(p)
        elif kind == "eswap":
            res.append(("eswap " + p, ESWAP_MIN, block))
        elif kind == "swap":
            res.append(("swap " + p, M.SWAP_MIN, block))
        elif kind == "run":
            res.append((p, M.minutes(by[p], True) + RUN_OVERHEAD_MIN, block))
        elif kind == "slot":
            if slot_live(p):
                res += [(i, M.minutes(by[i], True) + RUN_OVERHEAD_MIN, block) for i in p["ids"]]
            else:
                res.append(("slot " + p["name"] + " (reserve)", p["reserve"], block))
    return res


def totals(extra=False):
    t = {}
    for _, m, b in rows(extra):
        t[b] = t.get(b, 0.0) + m
    return t


def table_text(extra=False):
    out, cum, n = ["| # | block | step | minutes | cumulative |", "| --- | --- | --- | --- | --- |"], 0.0, 0
    for label, m, b in rows(extra):
        cum += m
        n += 1
        out.append("| %d | %s | %s | %.1f | %.1f |" % (n, b, label, m, cum))
    t = totals(extra)
    out.append("")
    order = sorted(t, key=lambda k: (k[0] != "A", k[0] != "B", int("".join(c for c in k if c.isdigit()) or 0), k))
    out.append("per block (min): " + ", ".join("%s %.0f" % (k, t[k]) for k in order) + "; all %.0f (%.1f h)" % (sum(t.values()), sum(t.values()) / 60))
    return "\n".join(out)


def lan_text():
    """r17-lan.txt: the two-console list (only for an owner with a second Vita): commands, not part of the sitting."""
    out = ["# r17 two-console LAN list (generated by gen_kit.py from sitting.py). ONLY if the owner has a second Vita; not part of r17-sitting.txt.",
           "# Both consoles on the same Wi-Fi as the Mac: export VITA_IP=<first Vita> VITA_IP2=<second Vita>; each leg is `bash session.sh lan <leg>` (about %d min a leg). Radio / Nearby runs and the by-hand items are in CONSOLE-PLAN-r17.md." % M.LAN_LEG_MIN]
    for leg in M.LAN_LEGS:
        out.append("lan\t%s\t%s" % (leg["id"], leg["title"]))
    out.append("# ---- SLOT delay19 (input delay; coming later): legs to be added here by kit20 when the coordinator sends the lines")
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    print(table_text())
