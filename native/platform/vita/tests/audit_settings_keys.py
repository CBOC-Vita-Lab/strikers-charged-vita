#!/usr/bin/env python3
"""Audit of the Settings screen's keys against the code that reads them.

For every choice of every row (np_settings.cpp, dumped by settings_dump) and every key it writes:
  1. the key must have an entry in READERS (a new key fails until its reader has been looked up and registered here),
  2. the reader file(s) named there must still contain the key's string literal next to an environment read (a key nobody reads, like the
     stale MSC_VITA_MSAA of the 1.1 anti-aliasing row, fails),
  3. the value written must pass the reader's own parse rule (WHAT: a transcription of the reader's code, with its file and function).
Also checks that every legacy key an option still owns has a reader, and (1.3 prune) that every RETIRED key (a key whose row or choice left the screen: kRetiredKeys in
np_settings.cpp) is written by no remaining choice, is listed here with the row it belonged to, and still has its reader: nothing was removed from the game.
Run from the repository root:  python3 native/platform/vita/tests/audit_settings_keys.py
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), *[".."] * 4))
TESTS = os.path.join(ROOT, "native/platform/vita/tests")
SRC = os.path.join(ROOT, "native/platform/vita/src")


def is_int(v, lo=None, hi=None):
    return re.fullmatch(r"-?\d+", v) is not None and (lo is None or int(v) >= lo) and (hi is None or int(v) <= hi)


def is_float(v, lo=None, hi=None):
    try:
        f = float(v)
    except ValueError:
        return False
    return (lo is None or f >= lo) and (hi is None or f <= hi)


def size_ok(v):
    m = re.fullmatch(r"(\d+)x(\d+)", v)
    return bool(m) and 160 <= int(m.group(1)) <= 960 and 120 <= int(m.group(2)) <= 544


# key: (reader files, validator, what the reader does with the value)
READERS = {
    "MSC_PREFETCH": (["native/src/NL/math.cpp", "renderer/src/vgx_rec.cpp"], lambda v: re.fullmatch(r"\d+", v) is not None and 0 < int(v) <= 31, "0", "sweep24: NpPrefetchInit(): strtol base 0, a bit mask; bits 1 2 8 16 are pld/pldw hints in the pose code (nlMath.h NpPrefetchLines), bit 4 the recorder's write pointer (vgx_rec.cpp); a hint changes no value (31 = all, exact: det trace and frame hashes equal)"),
    "MSC_VGL_MEMSITES": (["renderer/vita/vgx_memsite17.h", "dependencies/vitaGL/source/utils/mem_utils.c"], lambda v: v in ("0", "1"), "0", "memsite17 (default 0, diagnostic): 1 = vitaGL (patch 0041) keeps every pool block with its allocation site and kind (64 KiB table, a spinlock per allocation) and the [memsite] lines are written at kick-off, the first cut-scene and replay, the first refused allocation and the end of the run; 0 = one branch in the allocator wrappers, nothing stored"),
    "MSC_VGX_LADDER_ALIAS": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v == "0", "0", "LadderAliasOn(): EnvInt != 0 (with the picture hold) makes every one-sample or multisampled picture-size EFB target that fits the held size (held size, ladder sizes, made-ahead sizes) take its depth surface from one block per sample count at the held size (vitaGL 0044 vglFramebufferDepthShare) instead of a block of its own; the colour textures, the pass objects and the display keep their own memory; on the host GL the one-sample depth texture is shared the same way; 0 as before"),
    "MSC_VGL_PATCHER_BUF_KB": (["renderer/vita/vgx_host_vita.cpp"], lambda v: v.isdigit() and 256 <= int(v) <= 16384, "8192", "vram19 (default 8192 = as before): the sceGxm shader patcher's bufferMem in KiB. It is VIDEO memory (vitaGL gxm.c start_shader_patcher); the two code pools stay (MSC_VITA_PATCHER_MIB + 1) / 2 MiB of main memory each. The log's 'patcher buffer high-water' line suggests a size; lower it only after a console run shows no 'patching failed' and the warm gate passes"),
    "MSC_VITA_PB_LPDDR": (["renderer/vita/vgx_host_vita.cpp", "dependencies/vitaGL/source/gxm.c"], lambda v: v in ("0", "1"), "0", "vram19 (default 0, console only): 1 = SCE_GXM_INITIALIZE_FLAG_PB_LPDDR in a normal app (the 16 MiB parameter buffer in main memory, the video memory reserve 2 MiB instead of buffer + 2); a refusal by sceGxm falls back and vitaGL cuts the pool request back. The [vgx-vita] PB_LPDDR line says whether it was taken"),
    "MSC_VGX_DISPDEPTH": (["renderer/vita/vgx_host_vita.cpp", "dependencies/vitaGL/source/gxm.c"], lambda v: v in ("real", "nomem", "1"), "real", "vram19 (default real, console A/B after nodepth1): nomem = the display's depth/stencil surface (2.49 MiB of video memory) has no memory except while a system dialog is up (vitaGL patch 0042 makes one on the first swap with a dialog and frees it 90 swaps after the last); MSC_VITA_PB_MIB below 16 is refused with it"),
    "MSC_VITA_RT_SCENES": (["renderer/vita/vgx_host_vita.cpp"], lambda v: v in ("0", "1"), "0", "gpuscene20 (default 0, diagnostic): 1 = vitaGL (patch 0043) keeps the GPU time of each kind of scene (target size, display or not; 16 kinds and other) and a [gpu scenes] line follows each [rt] line; implies the vertex notifications (MSC_VITA_RT_SPLIT) and the GPU timers; 0 = nothing kept"),
    "MSC_VGX_PBEST": (["renderer/src/vgx_gpuscene20.h"], lambda v: v in ("0", "1"), "0", "gpuscene20 (default 0, diagnostic): 1 = per picture an ESTIMATE of the parameter-buffer bytes (vertices x (16 + 8 per texture coordinate + 4) + 12 a triangle), [pb est] every MSC_VGX_PBEST_WINDOW pictures; 0 = one flag test per draw"),
    "MSC_VGX_TEXCENSUS20": (["renderer/src/vgx_gpuscene20.h"], lambda v: v in ("0", "1"), "0", "gpuscene20 (default 0, diagnostic): 1 = [texlayout20] lines (live textures by DXT / uncompressed, mips, 128 texels or more) at kick-off, when a stadium's textures are freed and every MSC_VGX_TEXCENSUS20_WINDOW pictures; with MSC_VGX_FGCENSUS the 8 textures with most fetches; 0 = one flag test per upload, free and draw"),
    "MSC_POPIN_BLOCK": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v == "1", "0", "PopinOn(): EnvInt != 0 makes, from the match's first picture to MSC_POPIN_TAIL pictures after kick-off, every colour draw that could be left out for a program or shader not ready compile and link at once (walk-out pop-in); 0 leaves such a draw out as before (render only, no simulation)"),
    "MSC_POPIN_TAIL": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, 0, 3600), "90", "PopinWindowOpen(): EnvInt, pictures after kick-off that the MSC_POPIN_BLOCK window stays open"),
    "MSC_PICTURE_HOLD_SCENEPOL": (["renderer/src/vgx_backend_gl.cpp", "native/platform/vita/src/vita_plan.cpp"], lambda v: v in ("0", "1"), "0", "Init(): 1 gives held scenes (cut-scenes, replays, celebrations) their own policy (HoldSceneBegin): start at a level known to fit, shed in a few pictures, the fill governor only when it has no step left; needs AALAST; r15: off by default until the console decides (kit run scenepol1)"),
    "MSC_AI_NOCLOCK": (["native/platform/compat/np_aibudget.cpp"], lambda v: v == "1", "0", "Init(): getenv, 1 with MSC_AI_BUDGET=off: the script profiling clock reads 0 instead of the system clock (its only reader is the statistics)"),
    "MSC_SMOOTH_STEADY": (["native/platform/compat/np_smooth.cpp"], lambda v: is_int(v, 1, 64), "NpSmoothFrameBegin: EnvInt clamped 1..64 (ShowClock::SetSteady, code 8); pictures held exactly the period in a row before the show clock is trusted"),
    "MSC_SHADOW_LIVE": (["native/include/np/livetrades.h"], lambda v: v in ("full", "flat"), "NpShadowLiveFlat(): getenv; flat (or 1) is the soft ellipse in live play, anything else the game's shadow pictures; unset follows MSC_LIVE_TRADES"),
    "MSC_VITA_RENDER": (["renderer/vita/vgx_host_vita.cpp"], size_ok, "RenderSize(): sscanf %dx%d, 160x120..960x544, else ignored"),
    "MSC_PERFORMANCE_MODE": (["native/include/np/live_settings.h"], lambda v: v != "" and v[0] != "0", "NpPerformanceMode(): non-empty and not starting with 0 is on"),
    "MSC_FRAME_PACING": (["native/platform/compat/np_framepace.cpp"], lambda v: v == "0", "Enabled(): a value starting with 0 turns pacing off"),
    "MSC_FPS": (["native/platform/compat/np_fps.cpp"], lambda v: v.lower() in ("30", "45", "60", "auto", "unlocked", "unlock", "free", "game", "wii", "off", "0"), "Init(): named modes (45 is read as 30: the FRAME RATE choice of the s29-f45 test builds is gone)"),
    "MSC_FPS_REPLAY": (["native/platform/compat/np_fps.cpp"], lambda v: v.lower() in ("fit", "30", "auto"), "Init(): fit | 30 | auto"),
    "MSC_PAIR_RATE": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "0", "pacing13 (default 0): 1 = NpFpsPairWanted() follows the pair controller (np_pair_rate.h): the pair at 60 when the scene fits, an even 30 when it does not"),
    "MSC_PAIR_FAST_LEAVE": (["native/platform/compat/np_fps.cpp"], lambda v: v.isdigit() and 0 <= int(v) <= 16, "0", "lessons17 (default 0, needs MSC_PAIR_RATE=1): n of 1..16: the pair is also left when n of the last 16 pairs (at least 8 seen) cost more than MSC_PAIR_FAST_MS (Galaxy's 3 of 16)"),
    "MSC_PAIR_FAST_MS": (["native/platform/compat/np_fps.cpp"], lambda v: 20 <= float(v) <= 100, "33.0", "lessons17: the bound of MSC_PAIR_FAST_LEAVE in ms (default: the pair controller's leave limit)"),
    "MSC_PAIR_SCREENS": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "0", "lessons17 (default 0, needs MSC_PAIR_RATE=1): 1 = the display interval falls to the even 30 while a pause or home-menu bit is set (the pair controller keeps its state); the count of such pictures is logged either way"),
    "MSC_BG30_SCREENS": (["native/platform/compat/np_bg30.cpp"], lambda v: v in ("0", "1"), "0", "lessons17 (default 0, needs MSC_BG30=2): 1 = a picture made on a game-over / pending-state-change / wipe screen is never a light one; counted as 'screen' in the [bg30g] line either way"),
    "MSC_HEAD_STAT": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "0", "wave 17 (default 0, logging only): every 30 s one [head] line per scene class (play, replay, cut-scene, other): game-thread ms a picture p50/p95/max with and without a simulation step, render-thread and GPU p95, late pictures, relief, pictures over 16.7 ms by game / render / GPU (np_head17.h); atoi > 0 is on; needs MSC_LATE_CAUSE (default on) for the renderer lanes"),
    "MSC_FREEZE_BUTTON": (["native/platform/vita/src/np_freeze_button.h", "native/platform/vita/src/np_vita_main.cpp"], lambda v: v in ("0", "1"), "1", "galaxyport18 (default 1 in the player build, 0 in lab builds): Enabled(): unset or empty = the build's default, 0 = off, any other value on; on starts the msc_freezebutton thread (priority 191, 100 ms poll): L + R + Select held 3 s (after the first 20 s, never while the settings screen is open) writes a FATAL line with every thread's state and traps for the system's core dump (np_vita_crash.cpp FreezeTrap)"),
    "MSC_LATE_CAUSE": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "1", "lessons17 (default 1, logging only): one [late] line per 300 live pictures charging each late picture to the GPU, game CPU, render-thread CPU, front-end CPU or waits (np_late_cause.h); 0 = no line and no per-picture clock reads in the renderer"),
    "MSC_SIMT": (["native/src/Game/NpSimRun17.cpp"], lambda v: v in ("0", "1", "2"), "0", "block17 / overlap18 (default 0, plan-simthread17 stages 2a, 2c): ReadEnv(): 1 runs the simulation step loop on the np_sim thread while the game thread blocks (no overlap, no gain: a proof of the thread assumptions); 2 = the game thread draws the picture's window (world NPCs + crowd, MSC_SIMT_ORDER=B's stages) beside the step and joins (needs MSC_RENDER_RNG=2, MSC_SIMT_ORDER unset or B, frame send off or thread, else refused and the game runs as 0); refused in LAN / online / recorded games unless MSC_SIMT_LAN=1"),
    "MSC_VITA_CORE_SIM": (["native/platform/vita/src/vita_threads.cpp"], lambda v: re.fullmatch(r"[0-2]{1,3}", v) is not None, "", "pace18 (only with MSC_SIMT 1 or 2): CoreMask(kSim): digits 0-2, a set like 12 = cores 1 or 2, never 3 (refused with a log line); unset = MSC_SIMT=1 the game thread's core, MSC_SIMT=2 core 2; alias MSC_SIMT_CORE"),
    "MSC_SIMT_CORE": (["native/platform/vita/src/vita_threads.cpp"], lambda v: re.fullmatch(r"[0-2]{1,3}", v) is not None, "", "pace18: the alias of MSC_VITA_CORE_SIM (CoreMask(kSim) reads it when MSC_VITA_CORE_SIM is unset)"),
    "MSC_VITA_PRIO_SIM": (["native/platform/vita/src/vita_threads.cpp"], lambda v: re.fullmatch(r"(0x[0-9a-fA-F]+|\d+)", v) is not None, "", "pace18 (only with MSC_SIMT 1 or 2): SimPriority(): strtol base 0, the np_sim thread's priority; unset = MSC_SIMT=1 as the thread has it, MSC_SIMT=2 0x10000100-17 (one more urgent than the job workers); alias MSC_SIMT_PRIO"),
    "MSC_SIMT_PRIO": (["native/platform/vita/src/vita_threads.cpp"], lambda v: re.fullmatch(r"(0x[0-9a-fA-F]+|\d+)", v) is not None, "", "pace18: the alias of MSC_VITA_PRIO_SIM"),
    "MSC_VITA_CORE3_WATCH": (["native/platform/vita/src/vita_threads.cpp"], lambda v: v in ("0", "1"), "1", "pace18 (only when the fourth core was proven and is in use): StartCore3Watch(): a value starting with 0 turns the watch off; on, a 3 ms spell on core 3 every 2 s, two checks under 50 % of its time move the system core's threads back to cores 1-2 and mark the core unusable (NpVitaCore3::Watch)"),
    "MSC_LAT_BP_HELD": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "1", "BpHeldOn(): 1 (default) | 0: in a held scene (replay, cut-scene, walk-out) the back-pressure gate waits only on a backlog of more than maxDepth (4) pictures, the learned depth is kept and in force again at the cut to play (np_fps_bp.h SetHeld); 0 = the learned depth everywhere"),
    "MSC_LAT_BP_HELD_DEPTH": (["native/platform/compat/np_fps.cpp"], lambda v: v.isdigit() and 1 <= int(v) <= 4, "2", "BpCfgFromEnv(): 1..4, default 2 (held24): the back-pressure gate's depth in a held scene (MSC_LAT_BP_HELD=1): at most depth + 1 pictures in flight, so sound leads its picture by at most that many pictures; 4 = the first heldpipe23 build (5-6 in flight)"),
    "MSC_LAT_BP_SLEEP": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "0", "lessons17 (default 0): 1 = the back-pressure gate's room (refreshes it may wait) is taken less the pad late sleep that follows it (np_fps_bp.h GateRoomRefreshes)"),
    "MSC_CLOCK": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("debt", "slip"), "debt", "SlipFrame: debt (default, needs MSC_SLIP) | slip: the wave 9 bucket"),
    "MSC_LAT_BP": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "Init: 1 (default) | 0: a held cadence (30 or 60, single player) starts a picture, and reads the pad, only when at most 1-4 earlier pictures are unshown (np_fps_bp.h); 0 is the frames-in-flight limit of wave 10"),
    "MSC_FPS_PROBE_EVERY": (["native/platform/compat/np_fps.cpp"], lambda v: is_int(v, 1, 8), "1", "probe18 (default 1 = every picture, not in the plan): N of 1..8: the per-core probe (one sceKernelGetThreadInfo per thread) is read once in N pictures in auto mode; the pictures between hear the last window's per-picture mean; a LAN/online/recorded game, a scene change, a long picture, a policy change always read (ProbeSkipThisPicture)"),
    "MSC_TASK_EXEC_SKIP": (["native/src/NL/nlTask.cpp", "native/platform/vita/src/vita_plan.cpp"], lambda v: v in ("0", "1", "2"), "0", "gxp17 (default 0 in code, 1 in the Vita plan, EXACT): RunAllTasks does not read the clock for mExecutionTime (written, never read); 2 = host proof mutant (also skips the read that gives a task its time step: must change the det trace or frame hashes), never set in play"),
    "MSC_CPU22_INVLAZY": (["native/platform/compat/np_cpu22.cpp"], lambda v: v in ("0", "1", "2"), "0", "cpu22 (default 0 in code, 1 in the Vita plan from gxp17: EXACT game-thread cut, host proof in the gxp17 note): the shadow view matrix's inverse is made at the first read of gShadowInverseViewMatrix instead of at every SetGameObjectShadowViewMatrix; 0 = as before, 1 = the new way, 2 = both ways with the original answering and \"[cpu22] invlazy mismatches N\" logged (the console verify run must read 0)"),
    "MSC_CPU22_POWERUP": (["native/platform/compat/np_cpu22.cpp"], lambda v: v in ("0", "1", "2"), "0", "cpu22 (default 0 in code, 1 in the Vita plan from gxp17: EXACT game-thread cut, host proof in the gxp17 note): DrawablePowerup::Render keeps the 150 stadium drawable lookups (AVL tree plus drawable map) in a table that is dropped when a drawable is added or removed or the stadium changes; 0 = as before, 1 = the new way, 2 = both ways with the original answering and \"[cpu22] powerup mismatches N\" logged (the console verify run must read 0)"),
    "MSC_CPU22_REPLAY": (["native/platform/compat/np_cpu22.cpp"], lambda v: v in ("0", "1", "2"), "0", "cpu22 (default 0 in code, 1 in the Vita plan from gxp17: EXACT game-thread cut, host proof in the gxp17 note): GetReplayExcitement walks the replay ring once for both event masks instead of twice; 0 = as before, 1 = the new way, 2 = both ways with the original answering and \"[cpu22] replay mismatches N\" logged (the console verify run must read 0)"),
    "MSC_CPU22_BREAK": (["native/platform/compat/np_cpu22.cpp"], lambda v: v in ("1", "2", "3"), "0", "cpu22 (host proof mutant, ignored on the Vita): 1 perturbs the lazy shadow inverse, 2 stores a wrong drawable for idx&7==3 in the powerup cache, 3 uses another event mask in the merged replay walk; mode 2 must report mismatches"),
    "MSC_CPU24_PART": (["native/src/Game/Effects/ParticleSystem.cpp", "native/platform/compat/np_cpu24.cpp"], lambda v: v in ("0", "1", "2"), "0", "cpu24 (default 0 in code, 1 in the Vita plan: EXACT, host proofs in w8/cpu24.md): EvaluateParticleColour takes the keyframe index by a float truncation instead of floor() of a double, the stored timeFraction instead of a second divide, and skips the scale multiply and clamps while the three colour scales are 1.0f; 2 = verify (the original answers, \"[cpu24] part: ... mismatches N\", must read 0)"),
    "MSC_CPU24_TIGHTSTAT": (["native/src/Game/Effects/ParticleSystem.cpp", "native/platform/compat/np_cpu24.cpp"], lambda v: v in ("0", "1"), "0", "cpu24 (default 0 in code, 1 in the Vita plan: EXACT, host proofs in w8/cpu24.md): NpFxTightParticle makes the diagnostic sums sTs.quad / sTs.tight (a divide per sprite) only while MSC_FX_TIGHT_STAT is on; sTs.govBig, the governor's input, is unchanged"),
    "MSC_CPU24_NOTIFY": (["native/include/np/cpu24.h", "native/platform/compat/np_cpu24.cpp"], lambda v: v in ("0", "1"), "0", "cpu24 (default 0 in code, 1 in the Vita plan: frame hashes and det trace identical on the host, w8/cpu24.md): the mesh writers' End() report the vertex streams of one mesh as one written range (DCStoreRangeNoSync over their span) instead of one call each, three to four times fewer np_gx_notify_write rounds (a barrier and atomics each on the Vita); a notification only invalidates, so the span (alignment gaps included) is safe"),
    "MSC_CPU24_BREAK": (["native/platform/compat/np_cpu24.cpp"], lambda v: v in ("1", "4", "5"), "0", "cpu24 (host proof mutant, never set in play): 1 PART returns a wrong red channel for 1 colour in 64, 4 NOTIFY reports only the first stream of a mesh, 5 not at all; PART in mode 2 must count mismatches; NOTIFY must change the frame hashes"),
    "MSC_AI_NOCLOCK_BREAK": (["native/platform/compat/np_aibudget.cpp"], lambda v: v == "1", "0", "probe18 (host proof mutant, ignored on the Vita): with MSC_AI_NOCLOCK=1 the budget is 1.0 and the script clock counts, so the AI decision depends on the clock (the det trace must differ)"),
    "MSC_FPS_BETWEEN_COST": (["native/platform/compat/np_fps.cpp"], lambda v: v.lower() in ("free", "measured"), "Init: free (default) | measured: the free-run gate judges the cost at the free-run rate or the cost measured at the rate in force"),
    "MSC_FPS_BETWEEN": (["native/platform/compat/np_fps.cpp"], lambda v: v.lower() in ("30", "pace"), "Init(): 30 | pace"),
    "MSC_CADENCE45": (["native/platform/compat/np_fps.cpp", "native/platform/vita/src/vita_plan.cpp"], lambda v: v in ("0", "1"), "Init(): atoi != 0 with MSC_FPS unset or auto: the 45 cadence alone in live play (np_fps45.h; a developer switch, no Settings row; SMOOTH 60 in tiers uses the cadence itself)"),
    "MSC_F45_DEPTH": (["native/platform/compat/np_fps.cpp"], lambda v: is_int(v, 1, 3), "F45Announce(): atoi 1..3 (default 2): pictures unshown before a 45 frame may start while no slot is known (depth + 1 with one)"),
    "MSC_F45_LEAD": (["native/platform/compat/np_fps.cpp"], lambda v: v == "auto" or is_int(v, 3, 5), "F45Announce(): atoi 3..5 fixes the refreshes before its slot a 45 frame starts; anything else (auto, the default) learns it (np_fps45.h Lead45)"),
    "MSC_F45_LQ": (["native/platform/compat/np_fps.cpp"], lambda v: is_float(v, 0.5, 1.0), "F45Announce(): atof 0.5..1 (default 0.8): the start-to-ready quantile the 45 clock plans with"),
    "MSC_F45_FIT_MS": (["native/platform/compat/np_fps.cpp"], lambda v: is_float(v, 10.0, 22.3), "F45Announce(): atof 10..22.3 (default 20): the cost at 45 the fallback's proof accepts"),
    "MSC_LOCK60_TIERS": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "Init(): on with MSC_LOCK60 unless atoi == 0 (code default 1 with LOCKED 60): Smooth 60 in three tiers, 60 defended -> the strict 45 cadence -> the full-size 30 (np_fps_tier.h)"),
    "MSC_TIER_MEMORY": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "Init(): default 0; 1 remembers the tier a stadium settled at (Smooth 60 tiers) in <data dir>/np-tier-memory.txt and starts the next match there (pace13; console A/B first)"),
    "MSC_TIER_MEMORY_HOLD": (["native/platform/compat/np_fps.cpp"], lambda v: v.replace(".", "", 1).isdigit(), "Init(): seconds (default 60) a remembered start is held before the way up is tried (pace13)"),
    "MSC_TIER_MEMORY_FILE": (["native/platform/compat/np_fps.cpp"], lambda v: len(v) > 0, "TierMemPath(): path of the memory file, tests only (default <data dir>/np-tier-memory.txt)"),
    "MSC_GLOW_HALF_PLAY": (["native/include/np/scenecut.h", "renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("0", "1", "2"), "NpSceneGlowPlayParse() / hold setup: default 0; 1 the glow chain at half rate in live play as ladder step 1 (kept only if it bought GPU time), 2 always (tests, crops) (pace13)"),
    "MSC_TIER_DWELL": (["native/platform/compat/np_fps.cpp"], lambda v: v.replace(".", "", 1).isdigit(), "MakeAutoConfig(): seconds (default 0 = off) a fall from 60 by the miss / slow-mean rules waits after a rise under MSC_LOCK60_TIERS; a core or the GPU over budget still falls at once (pace13, host model tier_flap_unit.cpp; console A/B first)"),
    "MSC_TIER_RELAPSE": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "MakeAutoConfig() / Init(): 1 counts a fall within 20 s of a rise twice in the sticky history (60 and the 30 -> 45 return); default 0 (pace13)"),
    "MSC_LOCK60_HOLD": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "Init(): 1 with MSC_LOCK60_TIERS = hold60 (code default 0: the tiers as before): 60 judged on the delivered rate (50 a second over 5 s, 36 over 2 s), the climb on measured work (np_fps_ctl.h Config::Lock60Hold), 45 -> 30 on 15 % of 90 / 5 % of 360 and back sooner (Fallback45Config::Hold), the 60 clock's plan quantile 0.95"),
    "MSC_LOCK60_SPREAD": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "Init(): hold60b, default 1 with MSC_LOCK60_HOLD=1: at the 60 tier a late picture's catch-up is spread one step a picture while the debt clock owes at most a step (np_game_clock.h Spread60Cap); 0 off"),
    "MSC_LOCK60_USE45": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "Init() (p98, code default 0 = the owner's decision of 24 Nov): 0 = live play is 60 <-> 30 only, the 30 a steady 2 refreshes a picture; 1 = the 60 / 45 / 30 tiers as before (np_fps45.h Tier45Allowed; the 45 code stays compiled; MSC_FPS=45 is not affected)"),
    "MSC_LOCK60_USE50": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "Init() (f50, code default 0): 1 = the mid tier of live play is a STRICT 50 (5 pictures in 6 refreshes, 1-1-1-1-2, display-time game time): 60 -> 50 -> 30, the 30 only when the 50 cannot hold; implies the mid tier is allowed (USE45 not needed; USE50 wins when both); MSC_FPS=45 stays the 45; 0 = as before (np_fps45.h MidPattern / MidAllowed; [f50] lines; with MSC_LOCK60_P98 / TRIAL the 30 -> 50 return uses the trial)"),
    "MSC_LOCK60_P98": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "MakeAutoConfig() (p98, default 0): 1 = 60 is kept while over 10 s the repeated refreshes are at most 1.5 % and visible hitches at most one per 5 s; falls only when violated for 3 s with the worst 2 s burst forgiven (np_fps_ctl.h Config::p98); 0 = the hold60 slow / busy rules"),
    "MSC_P98_REPEAT_PCT": (["native/platform/compat/np_fps.cpp"], lambda v: v.replace(".", "", 1).isdigit(), "MakeAutoConfig() (p98): the share of repeated refreshes over the window that 60 tolerates (default 1.5)"),
    "MSC_P98_WINDOW": (["native/platform/compat/np_fps.cpp"], lambda v: v.replace(".", "", 1).isdigit(), "MakeAutoConfig() (p98): the window in seconds (default 10, 3..15)"),
    "MSC_P98_SUSTAIN": (["native/platform/compat/np_fps.cpp"], lambda v: v.replace(".", "", 1).isdigit(), "MakeAutoConfig() (p98): seconds the violation must last before 60 falls (default 3)"),
    "MSC_P98_HITCH_SECS": (["native/platform/compat/np_fps.cpp"], lambda v: v.replace(".", "", 1).isdigit(), "MakeAutoConfig() (p98): one visible hitch per this many seconds is tolerated (default 5)"),
    "MSC_LOCK60_TRIAL": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "MakeAutoConfig() (p98, default 0; needs MSC_LOCK60_P98=1): 1 = the relief tries 60 for 2 s after 4 s and at every cut back into play, backoff x2 to 20 s; with MSC_F45_RETRY=1 the fallback 30 tries the 45 the same way"),
    "MSC_LOCK60_TRIAL_DEFER": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "MakeAutoConfig() (flip25, default 1, a controller bug fix with a unit test): the trial of 60 by time honours DeferUp (no 60 trial while the 50 is on its relief: 30 -> 50 -> 60); 0 = as before"),
    "MSC_LOCK60_TRIAL_CALM": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "MakeAutoConfig() (flip25, default 1 since nxpace25, =0 restores; only acts with P98 + TRIAL, i.e. MSC_LOCK60_USE50=1): the trial of 60 by time waits after a scene change, is skipped while the predicted cost at 60 is over a share of a refresh and stops after N failed trials in a scene"),
    "MSC_LOCK60_TRIAL_CALM_FAILS": (["native/platform/compat/np_fps.cpp"], lambda v: v.isdigit(), "MakeAutoConfig() (flip25): failed 60 trials in a row per scene before the calm rule stops them (default 2)"),
    "MSC_LOCK60_TRIAL_CALM_FIT": (["native/platform/compat/np_fps.cpp"], lambda v: v.replace(".", "", 1).isdigit(), "MakeAutoConfig() (flip25): the predicted busiest-core cost at 60 over this share of a refresh skips the trial (default 0.90)"),
    "MSC_LOCK60_TRIAL_CALM_SETTLE": (["native/platform/compat/np_fps.cpp"], lambda v: v.replace(".", "", 1).isdigit(), "MakeAutoConfig() (flip25): seconds after a scene change before a 60 trial by time (default 6)"),
    "MSC_LOCK60_TRIAL_AFTER": (["native/platform/compat/np_fps.cpp"], lambda v: v.replace(".", "", 1).isdigit(), "MakeAutoConfig() (p98): seconds before the first trial (default 4)"),
    "MSC_LOCK60_TRIAL_SECS": (["native/platform/compat/np_fps.cpp"], lambda v: v.replace(".", "", 1).isdigit(), "MakeAutoConfig() (p98): seconds of a trial (default 2)"),
    "MSC_CROWD_SEED60": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "CrowdGovPicture() (why60, default 0, not in the plan): 1: a live picture at 60 that follows live pictures at another cadence (a trial of 60 after the relaxed 30 gave the crowd back) starts the crowd governor at the level the last 60 reached (np_crowdgov.h Seed) instead of walking down one level a second; render only"),
    "MSC_CROWD_FLOOR60": (["native/platform/compat/np_fps.cpp"], lambda v: is_int(v, 0, 5), "NpCrowdGovLevel() (core60, code default 0; the Vita plan: 1 with LOCKED 60 since wave 25): a live picture at 60 hands the render side at least this crowd governor level (npcg::FloorAt60); 30, replays, cut-scenes and menus get the governor's own level"),
    "MSC_CROWD_FLOOR60_LIVE": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "CrowdGovPicture() (int33, code default 0; Vita plan 1 in kLock60): keeps MSC_CROWD_FLOOR60 and MSC_AMORT_60 crowd amortization active across temporary 30-fps relief during Smooth 60 live open play (excludes Mega Strikes, Quality 30, held scenes, menus)"),
    "MSC_VITA_FBO_KEEP": (["renderer/vita/vgx_host_vita.cpp"], lambda v: v in ("0", "1"), "int26/int26b (console 6250/6405): vitaGL 0062 keeps FBO RT + depth across detach; code default 0, Vita plan 1 in kQuality (RT destroys -> 1, -6 MiB peak CDRAM)"),
    "MSC_P98_BACKOFF_MAX": (["native/platform/compat/np_fps.cpp"], lambda v: v.replace(".", "", 1).isdigit(), "MakeAutoConfig() (p98): the longest wait between trials (default 20)"),
    "MSC_LOCK60_FASTGRACE": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "MakeAutoConfig() (pace24, default 0): 1 = inside the cut-in's 6 s grace the 2 s fast window falls 60 only under MSC_LOCK60_PANIC_RATE (np_fps_ctl.h Config::holdFastGrace); 0 = as before"),
    "MSC_LOCK60_PANIC_RATE": (["native/platform/compat/np_fps.cpp"], lambda v: v.replace(".", "", 1).isdigit(), "MakeAutoConfig() (pace24): pictures a second under which the fast window still falls 60 inside the cut-in's grace with MSC_LOCK60_FASTGRACE=1 (default 24)"),
    "MSC_HOLD60_WALLSIM": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "MakeAutoConfig() (pace24, default 0, nxr4c checked it 25 Nov): 1 = the hold60 climb cost prices the simulation steps in wall ms like the frame (np_fps_ctl.h Config::holdWallSim, FrameIn::stepWallMs), not the thread's CPU ms; 0 = as before"),
    "MSC_F45_LATEONLY": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "Init() (pace24, default 0, with MSC_LOCK60_HOLD=1): 1 = the 45's fallback hears only pictures that missed their slot (not planned holds) plus the delivered rate (np_fps45.h Fallback45Config::rateWindow); 0 = as before"),
    "MSC_F45_RATE_MIN": (["native/platform/compat/np_fps.cpp"], lambda v: v.replace(".", "", 1).isdigit(), "Init() (pace24): pictures a second under which the 45 falls to 30 over 135 pictures with MSC_F45_LATEONLY=1 (default 37)"),
    "MSC_F45_RETRY": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "Init() (trap24, default 0, with MSC_LOCK60_HOLD=1): 1 = the fallback 30's proof may be judged on the MEAN estimated cost of its last 120 pictures once the hold has been over MSC_F45_RETRY_SECS (np_fps45.h Fallback45Config::relaxAfter); 0 = as before"),
    "MSC_F45_RETRY_SECS": (["native/platform/compat/np_fps.cpp"], lambda v: v.replace(".", "", 1).isdigit(), "Init() (trap24): seconds after the hold the mean proof may pass (default 20, at least 1) with MSC_F45_RETRY=1"),
    "MSC_F45_RETRY_MEAN": (["native/platform/compat/np_fps.cpp"], lambda v: v.replace(".", "", 1).isdigit(), "Init() (trap24): the mean estimated cost at 45 in ms the mean proof allows (default 18, 8..22) with MSC_F45_RETRY=1"),
    "MSC_LOCK60_SPREAD_CAP": (["native/platform/compat/np_fps.cpp"], lambda v: v.replace(".", "", 1).isdigit(), "Init() (pace24, default 0 = 20): ms 20..60 of game time owed up to which the 60's catch-up stays one step a picture (np_game_clock.h Spread60Cap debtCapMs; 40 = two steps), with MSC_LOCK60_SPREAD"),
    "MSC_F60_LQ": (["native/platform/compat/np_fps.cpp"], lambda v: v.replace(".", "", 1).isdigit(), "Init(): 0.5..1, the 60 tier's show-time clock's plan quantile of start-to-ready refreshes (default 0.95 with MSC_LOCK60_HOLD=1, else MSC_F45_LQ's 0.8)"),
    "MSC_LOCK60_SHOWCLOCK": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "Init(): 0 leaves the 60 tier's game time as it was (default 1 with MSC_LOCK60_TIERS: the vblank each picture is shown on)"),
    "MSC_LOCK60_STICKY": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "MakeAutoConfig(): atoi != 0 is on (code default 1 with LOCKED 60): after a relief to the full-size 30 a gameplay scene stays there (np_fps_ctl.h Config::sticky)"),
    "MSC_LOCK60_STICKY_HOLD": (["native/platform/compat/np_fps.cpp"], lambda v: is_int(v, 0, 600), "MakeAutoConfig(): seconds of the minimum hold at 30 (default 25)"),
    "MSC_LOCK60_STICKY_PROOF": (["native/platform/compat/np_fps.cpp"], lambda v: is_int(v, 1, 120), "MakeAutoConfig(): seconds of headroom proof before 60 returns (default 12)"),
    "MSC_LOCK60": (["native/platform/compat/np_fps.cpp", "renderer/src/vgx_backend_gl.cpp", "native/platform/vita/src/vita_plan.cpp"], lambda v: v in ("0", "1"), "atoi != 0 is on (1.3 LOCKED 60)"),
    "MSC_LOCK60_UPHARD": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_float(v, 0.5, 1.0), "Lock60Init(): atof 0.5..1.0 (default 0.90, kLock60 0.97): single-frame predicted-share ceiling during step-up proof"),
    "MSC_LOCK60_PLAY_START": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, -1, 15), "L60SetKind()/L60PlanTour(): EnvInt (default -1, kLock60 2): starting Smooth-floors rung for live play kPlay"),
    "MSC_LOCK60_PROBEAT": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_float(v, 0.70, 1.30), "Lock60Init(): atof 0.70..1.30 (default 0.95, kLock60 1.06): single-frame budget share ceiling during post-climb probation"),
    "MSC_LOCK60_LATEAT": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_float(v, 0.90, 1.30), "Lock60Init(): atof 0.90..1.30 (default 1.00, kLock60 1.08): single-frame budget share ceiling for immediate step-down"),
    "MSC_LOCK60_HELD_FULL": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("0", "1"), "L60SetKind()/L60Frame(): EnvInt != 0 starts menu and Mega Strike scenes at 960x544 1x (cut-scenes/replays at 768x544 1x) and evaluates held-30 non-gameplay scenes against the 33.3 ms budget"),
    "MSC_DYNRES": (["renderer/src/vgx_backend_gl.cpp", "native/platform/vita/src/vita_plan.cpp"], lambda v: v in ("off", "0"), "DynInit(): off or 0 turns it off (vita_plan also leaves the GPU timers off)"),
    "MSC_DYNRES_MAX": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_float(v, 1.0, 1.2), "DynInit(): atof, first level whose scale is <= the value"),
    "MSC_DYNRES_FG": (["renderer/src/vgx_skin.cpp"], lambda v: v in ("0", "1"), "FgRequested(): 0 / off is off, anything else is on (the build sets 1)"),
    "MSC_VGX_MSAA": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("0", "2", "4"), "MsaaProbe(): EnvInt, < 2 is off, >= 4 is 4 samples, 2..3 is 2 (the build sets 4)"),
    "MSC_PICTURE_HOLD_RESERVE_KIB": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v.isdigit(), "Init(): KiB of video memory that must be free at kick-off, else targets are let go before the match (default 12288)"),
    "MSC_PICTURE_HOLD_MSAA": (["renderer/src/vgx_backend_gl.cpp", "native/platform/vita/src/vita_plan.cpp"], lambda v: v in ("0", "2", "4"), "Init(): EnvInt, >= 4 is 4 samples, 2..3 is 2, below is none; only with MSC_PICTURE_HOLD (the build sets 2; the ANTI-ALIASING row's 4X writes 4; OFF / 2X ONLY write MSC_VGX_MSAA=0 / 2, which the plan maps onto it)"),
    "MSC_HDTEX": (["renderer/src/vgx_hdtex.cpp"], lambda v: v in ("0", "1", "2"), "Install(): EnvInt; <= 0 is off"),
    "MSC_FX_LOWRES_SCENES": (["renderer/src/vgx_backend_gl.cpp"], lambda v: all(t in ("all", "other", "play", "replay", "nis") for t in v.split(",")), "FxSceneMask(): comma list of scene classes"),
    "MSC_FX_LOWRES_MINPX": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, 0), "FxMinPx(): EnvInt"),
    "MSC_FX_LOWRES": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("0", "off", "4", "always"), "FxDiv(): 0 or off = no effects pass"),
    "MSC_VITA_EDGE_AA": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("0", "1"), "EnvInt: 0 is off"),
    "MSC_VITA_CORE3": (["native/platform/vita/src/vita_threads.cpp"], lambda v: v in ("0", "1"), "probe, then moves threads; 0 or absent does nothing"),
    "MSC_VITA_CORE3_SEND": (["native/platform/vita/src/vita_threads.cpp"], lambda v: v in ("0", "1", "2"), "probe, then moves the frame send (2: and the vitaGL collector); 0 or absent does nothing"),
    "MSC_VITA_CORE3_DISC": (["native/platform/vita/src/vita_plan.cpp", "native/platform/vita/src/vita_threads.cpp"], lambda v: v in ("0", "1"), "0", "T8 (only with MSC_VITA_CORE3=1): 1 lets the disc and save workers (dvd-io, dvd-open, dvd-keep, nand-io) use core 3 (vita_plan.cpp writes NP_VITA_CORE_IO=3; the rules' FilterMask allows them); 0 or absent: they stay on their plan core (Galaxy's stage-load freeze suspect)"),
    "MSC_VITA_CORE3_RULES": (["native/platform/vita/src/vita_plan.cpp", "native/platform/vita/src/vita_threads.cpp"], lambda v: v in ("0", "1"), "1", "T8: with MSC_VITA_CORE3=1 the core 3 rules apply (np_vita_core3.h: short or blocking threads only, no spinning, audio priority floor, everything off while an online or LAN session is up); 0 gives back the 1.3 placement (disc and save workers on core 3, frame send and collector as MSC_VITA_CORE3_SEND says) for an A/B"),
    "MSC_VITA_CORE3_AUDIO_PRIO": (["native/platform/vita/src/vita_threads.cpp", "native/platform/vita/src/np_vita_audio.cpp"], lambda v: re.fullmatch(r"(0x[0-9a-fA-F]+|\d+)", v) is not None, "128", "T8 (core 3 rules on): Core3PrioFloor(): the most urgent priority a thread may have while it is on core 3, 64..191 or 0x10000100 +- offset; default 128 (the output thread's own); 64 gives Galaxy's top priority back"),
    "MSC_PLAN": (["native/platform/vita/src/vita_plan.cpp"], lambda v: v in ("Cn", "A", "S", "auto"), "Apply(): first letter A / S / Cn"),
    "MSC_VITA_UPSCALE": (["renderer/vita/vgx_host_vita.cpp"], lambda v: v in ("fast", "off", "0", "blit", "linear", "bilinear", "1", "sharp", "2", "sharp+", "unsharp", "3", "smooth", "smooth+", "4"), "PictureModeFromString()"),
    "MSC_VITA_SHARPEN": (["renderer/vita/vgx_host_vita.cpp"], lambda v: is_float(v, 0.0, 1.5), "atof, clamped to 0..1.5"),
    "MSC_VITA_ANISO": (["renderer/vita/vgx_host_vita.cpp"], lambda v: v in ("0", "2", "3"), "exactly one character, 2 or 3 sets the filter; anything else (0) leaves plain trilinear (the build sets 3)"),
    "NP_WIDESCREEN": (["native/platform/vita/src/np_vita_wire.cpp", "native/platform/sdk/src/os_core.cpp"], lambda v: is_int(v), "atoi != 0 is wide"),
    "MSC_LAN_MODE": (["native/platform/sdk/net/np_menu_flow.cpp"], lambda v: v in ("wifi", "online"), "ChosenMode(): wifi, else ad hoc; np_menu_flow.cpp NpMenuWanted(): online starts the online entry (NpLanRadioPoll never falls back to the radio)"),
    "MSC_LAN_INPUT_DELAY": (["native/platform/sdk/net/np_lan.cpp"], lambda v: v == "auto" or is_int(v, 1, 20), "NpLanChooseInputDelay(): auto (from the measured round trip) or ticks 1..20, unset = the game's own"),
    "MSC_LAN_DIALOG": (["native/platform/sdk/net/vlink_vita_radio.cpp"], lambda v: v in ("conn", "create", "join"), "dialog mode: conn | create | else join"),
    "MSC_LAN_RADIO_FIX": (["native/platform/sdk/net/np_galaxy16.h"], lambda v: v.isdigit() and 0 <= int(v) <= 15, "ParseRadioFix(): decimal 0..15, bits 1 soft close, 2 stray receive errors, 4 any source port, 8 log lines; anything else is 0 (default)"),
    "MSC_LAN_GROUP": (["native/platform/sdk/net/np_galaxy16.h"], lambda v: v in ("0", "1"), "LanGroup(): 0 = no group name in the NetCheck dialog, anything else (default 1) as in 1.2"),
    "MSC_LAN_TIMEOUT_MS": (["native/platform/sdk/net/np_galaxy16.h"], lambda v: v.isdigit() and 0 <= int(v) <= 3600000, "LanTimeoutUs(): the NetCheck dialog's time-out, default 60000"),
    "MSC_MP_KEEP_AWAKE": (["native/platform/sdk/net/vlink_vita.cpp"], lambda v: v in ("0", "1"), "KeepAwakeOn(): getenv; 0 = no power tick (the system dims and suspends in a lobby, queue or session as before), anything else (default 1) = np_keep_awake.h ticks sceKernelPowerTick once a second while a LAN or online session, lobby, queue or invite wait exists (never in single player)"),
    "MSC_NET_KEEPAWAKE_MS": (["native/platform/sdk/net/so_net.cpp"], lambda v: is_int(v, 0, 5000), "wake14: FingerprintInit: ms, 0 = off (the default, kKeepAwakeDefaultMs), 10..5000 (below 10 is 10): while a peer link (a console we sent a unicast to in the last 10 s) carries nothing for this long, vlink.cpp sends it one header-only filler frame (dst port 0, dropped by the receiving link before any socket); never in a match that sends every tick"),
    "MSC_LINK_GAPLOG": (["native/platform/sdk/net/so_net.cpp"], lambda v: is_int(v, 0, 100000), "wake14: FingerprintInit: ms threshold, 0 / unset = off: logs every quiet spell of a peer link of at least that long, in each direction, with the session phase (np_lan.cpp NpLanGapPhase)"),
    "MSC_LAN_DRAIN": (["native/platform/sdk/net/np_delay19.h"], lambda v: v in ("0", "1"), "ParseDrain(): unset, empty or 0 is off (default); anything else reads the already-arrived datagrams once a frame before the step loop (receive loop only)"),
    "MSC_LAN_RETRY_US": (["native/platform/sdk/net/np_delay19.h"], lambda v: is_int(v, 0, 8000), "ParseRetryUs(): atoi clamped to 0..8000, unset or 0 = off (default): microseconds a starved step may wait per frame, receiving only"),
    "MSC_MP_BETA": (["native/platform/sdk/net/np_mpbeta.h"], lambda v: v in ("0", "1"), "On(): EnvOff; 0 = the 1.3.0 multiplayer labels (no BETA tag, no notice, no result line, the game's own sync-error text), anything else (default) = the public beta's labels and texts (mpbeta27; texts only)"),
    "MSC_MP_BETA_NOTICE": (["native/platform/sdk/net/np_mpbeta.h"], lambda v: v in ("0", "1", "always"), "NoticeMode(): getenv; 0 never, always every visit, anything else (default) once (marker mp-beta-seen.txt); unset in a scripted run (MSC_INPUT_SCRIPT) never"),
    "MSC_MP_REPORT_ENTRY": (["native/platform/sdk/net/np_mpbeta.h"], lambda v: v in ("0", "1"), "ReportEntryOn(): EnvOff; 0 = the mode choice's third button is CANCEL as in 1.3.0, anything else (default) = REPORT A PROBLEM"),
    "MSC_MP_REPORT_PLUS": (["native/platform/sdk/net/np_mpdiag.cpp"], lambda v: v in ("0", "1"), "ReportPlusOn(): getenv; 0 = the 1.3.0 report (no session trail sections, the offer only with an online account), anything else (default) = the trail sections and the offer without an account (saved to a file)"),
    "MSC_ONLINE_BUILD_HEADER": (["native/platform/sdk/dwc/np_online_api.cpp"], lambda v: v in ("0", "1"), "GetClientInfo(): getenv; 0 = no X-Msc-Build header (1.3.0), anything else (default) = the link fingerprint (NpLanFingerprint, 8 hex digits) on every request of the online service, so the queue pairs the same build only (server QUEUE_SAME_BUILD)"),
    "MSC_NET_WIFI_GRACE_S": (["native/platform/sdk/net/vlink_vita.cpp"], lambda v: is_int(v, 2, 15), "graceLooks: getenv/atoi clamped 2..15 (default 2, as before): looks a second apart that Wi-Fi may be away inside a session before the link is declared lost"),
    "MSC_MP_CRASH_MARK": (["native/platform/sdk/net/np_mpdiag.cpp"], lambda v: v in ("0", "1"), "MarkOn(): getenv; 0 = no mp-session.txt marker and no check at the next start, anything else (default) = the marker of a session that was not left offers a report"),
    "MSC_NET_KEEP_STACK": (["native/platform/sdk/net/np_mpfix18.h"], lambda v: v in ("0", "1"), "ParseKeepStack(): 0 = sceNet is shut down after a Nearby session as in 1.2, anything else (default) leaves the stack up for HTTPS and the online socket"),
    "MSC_DOF": (["native/src/Game/Render/depthoffield.cpp"], lambda v: v[:1] in ("0", "1", "l"), "DepthOfFieldMode(): first character 0 (or 'of...') turns the effect off (the build sets it since ship26), 'l...' is lazy (the row's ON writes it), anything else (1) the game's own"),
    "MSC_RULE_NOMEGA": (["native/platform/compat/np_gamefixes.cpp"], lambda v: v not in ("", "0", "off", "none"), "NpRuleNoMegaDecide(): on unless empty, 0, off or none"),
    "MSC_LANGUAGE": (["native/platform/sdk/src/os_core.cpp"], lambda v: v in ("en", "es", "fr"), "os_core.cpp config: es, fr or en by the first two characters"),
    "MSC_REDUCED_MOTION": (["native/include/np/live_settings.h"], lambda v: v != "" and v[0] != "0", "NpShakeScale(): non-empty and not starting with 0 is on (a quarter of the camera shake)"),
    "MSC_GAME_FIXES": (["native/platform/compat/np_gamefixes.cpp"], lambda v: v in ("all", "none") or re.fullmatch(r"[0-7]", v) is not None, "NpGameFixesDecide(): all, a digit mask, or names (net, electro, nk)"),
    "MSC_UNLOCK_CAPTAINS": (["native/platform/compat/np_unlocks.cpp"], lambda v: v in ("0", "1"), "first character 0 is off"),
    "MSC_UNLOCK_STADIUMS": (["native/platform/compat/np_unlocks.cpp"], lambda v: v in ("0", "1"), "first character 0 is off"),
    "MSC_CONTROLS": (["native/platform/sdk/input/np_controls.h"], lambda v: v in ("classic", "modern"), "ConfiguredLayout(): classic | CLASSIC | 0, else modern"),
    "MSC_REAR_TOUCH": (["native/include/np/live_settings.h"], lambda v: v in ("0", "1"), "RearEnabled(): first character 0 is off"),
    "NP_MENU_NAV": (["native/platform/sdk/input/np_menu_nav.cpp"], lambda v: v == "0", "Enabled(): a value starting with 0 is off"),
    "NP_TOUCH_POINTER": (["native/platform/sdk/input/np_game_input.cpp", "native/platform/sdk/src/input_native.cpp"], lambda v: v == "0", "first character 0 is off (atoi 0 in input_native)"),
    "MSC_TOUCH_SAVE": (["native/platform/sdk/input/np_touch.cpp", "native/platform/sdk/input/np_control_table.h"], lambda v: v == "0", "mode_touch_enabled(): first character 0 is off"),
    "MSC_TOUCH_METER": (["native/platform/sdk/input/np_touch.cpp", "native/platform/sdk/input/np_control_table.h"], lambda v: v == "0", "mode_touch_enabled(): first character 0 is off"),
    "MSC_TOUCH_SHAKE": (["native/platform/sdk/input/np_touch.cpp", "native/platform/sdk/input/np_control_table.h"], lambda v: v == "0", "mode_touch_enabled(): first character 0 is off"),
    "MSC_MEGA_AIM": (["native/platform/sdk/input/np_controls.h"], lambda v: v == "rear", "RearSaveEnabled(): rear"),
    "MSC_MEGA_POINTER_SPEED": (["native/platform/sdk/input/np_controls.h"], lambda v: re.fullmatch(r"[1-4]", v) is not None, "MegaPointerGain(): one digit 1..4"),
    "MSC_NET_RELAY_ONLY": (["native/platform/sdk/net/vlink_online.cpp"], lambda v: v == "1", "BuildConfig(): EnvInt != 0 is relay-only (the match goes through the relay server, no direct path, no address published)"),
    "MSC_DYNRES_MIN60": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_float(v, 0.7, 1.0), "DynInit(): atof; the floor at 60 is the last rung whose size is at least the value, at most two rungs below the ordinary floor (0.70 is on the table)"),
    "MSC_FX_GLOW": (["native/include/np/scenecut.h"], lambda v: v in ("half", "full"), "NpSceneGlowHalfParse(): exactly half, anything else (the row writes full) is the game's chain"),
    "MSC_SHADOW_EVERY": (["native/include/np/scenecut.h"], lambda v: is_int(v, 1, 4), "NpSceneShadowEveryParse(): atoi clamped to 1..4"),
    "MSC_REPLAY_FARLOD": (["native/include/np/scenecut.h"], lambda v: is_float(v, 0.01), "NpSceneFarLodParse(): atof, > 0"),
    # 1.3 defaults' rows (their OFF choices)
    "MSC_FX_GOV": (["native/src/Game/Effects/ParticleSystem.cpp", "renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("0", "1"), "atoi / EnvInt != 0 is on"),
    "MSC_FX_GOV_MEGA": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("0", "1"), "EnvInt != 0, acts only with MSC_FX_GOV"),
    "MSC_SHADOW_PLAY": (["native/src/Game/Drawable/DrawableCharacter.cpp"], lambda v: is_int(v, 2, 4), "NpShadowPlayParse(NpAmortEnvInt): clamped to 2..4, 2 is the game"),
    "MSC_MENU_STILL": (["native/src/Game/Render/StillBackdrop.cpp"], lambda v: v in ("0", "1"), "NpStillParse(): exactly 1 is on"),
    "MSC_PAUSE_STILL": (["native/src/Game/Render/StillBackdrop.cpp"], lambda v: v in ("0", "1"), "NpStillParse(): exactly 1 is on"),
    "MSC_VGX_DLWELD": (["renderer/src/vgx_dlcache.cpp"], lambda v: v == "1", "DlWeldOn(): EnvInt != 0, each distinct vertex of a display list written once at the promotion and the strip/fan indices remapped (exact; Vita plan 0)"),
    "MSC_VGX_DLREFRESH": (["renderer/src/vgx_dlcache.cpp"], lambda v: v in ("0", "1"), "RefreshOn(): EnvInt != 0, a display list the game rewrites before every draw is kept and only its changed arrays are decoded again (exact; Vita plan 1)"),
    "MSC_VGX_FEPAR": (["renderer/src/vgx_fepar.inc", "renderer/src/vgx_state.cpp"], lambda v: v in ("0", "1", "2"), "FeparMode(): 0 off, 1 the worker decodes plain stream draws, 2 verify (the player decodes, the worker's copy is compared)"),
    "MSC_VGX_FEPAR_KEEP": (["renderer/src/vgx_fepar.inc", "renderer/vita/vgx_host_vita.cpp"], lambda v: is_int(v, 0, 7), "KeepBits() (core60, code default 0; the Vita plan: 7 with LOCKED 60 since wave 25): bit 1 the worker on the system core runs above the core watch's spell, bit 2 the cost rule (mean join wait / 12 stalls over 3 ms in 128 joins) instead of 3 late joins in 48, bit 4 a retired worker returns to the system core after MSC_VGX_FEPAR_KEEP_RETRY_S s (doubling, MSC_VGX_FEPAR_KEEP_TRIES)"),
    "MSC_VGX_FEPAR_KEEP_RETRY_S": (["renderer/src/vgx_fepar.inc"], lambda v: is_int(v, 1, 3600), "KeepRetryS(): seconds before the first return to the system core, clamped to 1..3600"),
    "MSC_VGX_FEPAR_KEEP_TRIES": (["renderer/src/vgx_fepar.inc"], lambda v: is_int(v, 0, 16), "KeepTries(): returns allowed, clamped to 0..16"),
    "MSC_VGX_TEXSAME": (["renderer/src/vgx_texcache.cpp", "renderer/src/vgx_play.cpp"], lambda v: v in ("0", "1", "2"), "TexSameMode(): 0 every texture bind dirties the bindings, 1 a bind of the slot's own object with no write notification leaves them clean, 2 verify (dirty as before, each resolution compared)"),
    "MSC_VGX_MTXSPLIT": (["renderer/src/vgx_draw.cpp"], lambda v: v in ("0", "1", "2"), "MtxSplitMode(): 0 BuildMtx rebuilds every matrix block, 1 only the blocks the written rows reach, 2 verify (the full build runs after and must change nothing)"),
    "MSC_VGX_LA88": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("0", "1", "2", "3"), "Caps: EnvInt != 0 two bytes, >= 2 I4/I8 one byte, >= 3 mipmapped I/IA too (tile24)"),
    "MSC_VGX_CMPRA": (["renderer/src/vgx_texcache.cpp"], lambda v: v in ("0", "1", "2"), "EnvInt: 0 decoded, 1 DXT5, 2 DXT5 within MSC_VGX_CMPRA_TOL"),
    "MSC_VGX_TEX16": (["renderer/src/vgx_texcache.cpp"], lambda v: v in ("0", "1", "2"), "EnvInt: 0 off, 1 textures without mips, 2 also mipmapped"),
    "MSC_VGX_LODBIAS": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, -8, 0), "ParseLodBias (vgx_picture_math.h): 0 to -8, anything else 0"),
    "MSC_VITA_SHAPE": (["renderer/vita/vgx_host_vita.cpp"], lambda v: v == "1", "exactly 1, full-screen picture only"),
    "MSC_VGX_HUDNATIVE": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v == "1", "the first character is not 0"),
    "MSC_PICTURE_HOLD": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("0", "off", "full", "1", "native") or size_ok(v), "Init(): hold::ParseSize; 0 / off / empty is off, full = 960x544 held, 1 / native = 640x448, WxH any (the build sets full; FULL PICTURE OFF writes 0)"),
    "MSC_VGX_GXM_DIRECT": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("0", "1"), "EnvInt != 0, and only when vitaGL has vglDrawElementsDirect"),
    "NP_POINTER_SPEED": (["native/platform/sdk/input/np_game_input.cpp", "native/platform/sdk/src/input_native.cpp"], lambda v: is_float(v, 0.01), "atof, > 0"),
}
# keys vita_plan.cpp gives a default to ("quality and timing switches on by default", 1.2) and that no Settings row writes: reader files, whether the reader takes the default
# value as ON, the value that turns the item off (the A/B of tools/console-1.2 writes it), a transcription of what the reader does with that value
DEFAULTED = {
    "MSC_BLEND_ASYNC": (["native/src/Game/RenderSnapshot.cpp"], lambda v: v in ("0", "1"), "gt27: live-play blend of the characters / power-ups on the helper lane, joined in RenderSnapshot::Blend; console int8 A/B 1-step p50 -1 ms"),
    "MSC_VGX_DLWELD": (["renderer/src/vgx_dlcache.cpp"], lambda v: v == "1", "0", "weld17: DlWeldOn(): EnvInt != 0, display-list vertices welded at the promotion (exact)"),
    "MSC_PICTURE_FLOORS": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v == "1", "0", "Init(), DynBuildLevels(): 1 cuts every ladder rung below the owner's floors (Quality 864x488, Smooth 60 800x456 in pixels: vgx_floors.h) and holds a steady 30 at the full size when 60 cannot hold at its floor; the build sets 1, 0 is the old ladders"),
    "MSC_VGX_ASYNC_HEAPONLY": (["renderer/vita/vgx_host_vita.cpp"], lambda v: v == "1", "0", "gxvgx14: VgxHostAsyncWorkerPlace: the shader worker's vglMalloc family takes newlib's heap only (wrappers at the link; exact while the heap has room)"),
    "MSC_VGX_DLREFRESH": (["renderer/src/vgx_dlcache.cpp"], lambda v: v == "1", "0", "dlrefresh24: RefreshOn(): EnvInt != 0, rewritten display lists kept and refreshed in place instead of bypassed (exact)"),
    "MSC_VGX_FEPAR": (["renderer/src/vgx_fepar.inc"], lambda v: v == "1", "0", "fepar25: FeparMode() != 0, the vertex decode of plain stream draws on a worker thread (exact)"),
    "MSC_VGX_TEXSAME": (["renderer/src/vgx_texcache.cpp"], lambda v: v == "1", "0", "fe26: TexSameMode() != 0, a repeated texture bind leaves the bindings clean (exact)"),
    "MSC_VGX_MTXSPLIT": (["renderer/src/vgx_draw.cpp"], lambda v: v == "1", "0", "fe26: MtxSplitMode() != 0, matrix blocks rebuilt from the rows written (exact)"),
    "MSC_STDIO_LOCK": (["native/platform/vita/src/vita_log.cpp"], lambda v: v == "1", "0", "crash26b: ApplyStdioMode(): anything but 0 sends the wrapped printf / fputs / fwrite / putc family on stdout / stderr around the FILE (np_log_stdio.h); 0 is the 1.2 path through the FILE"),
    "MSC_AI_NOCLOCK": (["native/platform/compat/np_aibudget.cpp"], lambda v: v == "1", "0", "probe18: the script profiling clock reads 0 while MSC_AI_BUDGET=off (statistics only; det identical)"),
    "MSC_VGX_CLEARVEH": (["renderer/src/vgx_warble19.h"], lambda v: v in ("0", "1"), "0", "warble19: the vehicle clear of the shimmer chain folded into its scene (exact; Lava Pit, Sand Tomb)"),
    "MSC_VGX_EFBSWAP_NEXT": (["renderer/src/vgx_warble19.h"], lambda v: v in ("0", "1"), "0", "warble19: the EFB colour handed to the next scene instead of a copy (exact; refused under 6 MiB free)"),
    "MSC_VGX_PASSISO_COVER": (["renderer/src/vgx_draw.cpp"], lambda v: v in ("0", "1"), "0", "warble19: a pass that covers the picture isolated without a copy (exact; may not act on the Vita)"),
    "MSC_WARBLE_LITE": (["renderer/src/vgx_warble19.h"], lambda v: is_int(v, 0, 3), "0", "warble19: bit 1 the shimmer map every second picture in the 60 mode, bit 2 the half-size map at Sand Tomb"),
    "MSC_FS_REFRESH_PREFETCH": (["native/src/NL/gl/glRenderList.cpp"], lambda v: v in ("0", "1"), "0", "gt27: GLRefreshPacketSnapshots(): 1 prefetch hints for the next records' sources while one is copied (the bytes copied are the same)"),
    "MSC_SIM26_BP": (["native/src/ode/collision_space.cpp"], lambda v: v in ("1", "2"), "0", "sim26: NpSim26BpMode(): 1 the rows of a character's bone run test only the run's candidate list (union tested once against every later geom; exact, same survivors in the same order), 2 verify"),
    "MSC_SIM26_BONES": (["native/src/Game/Physics/PhysicsCharacterBase.cpp"], lambda v: v in ("1", "2"), "0", "sim26: NpSim26BonesMode(): 1 UpdatePose's arithmetic on the pose lane, pass 3 checks every input bit for bit and writes (exact), 2 verify"),
    "MSC_SIM26_POSEORDER": (["native/src/Game/Physics/PhysicsPose.cpp"], lambda v: v == "1", "0", "sim26: only 1 is on: the lane list of the poses longest first (most bone volumes); timing only, the poses are independent"),
    "MSC_SIM26_NETGRAB": (["native/src/Game/Drawable/DrawableNetMesh.cpp"], lambda v: v in ("1", "2"), "0", "sim26: NpSim26NetGrabMode(): 1 the nets' tri-strip / texcoord copy skipped while their load epoch, source and buffers are unchanged (same bits), 2 verify"),
    "MSC_SIM26_ISLANDS": (["native/src/ode/util.cpp"], lambda v: v in ("1", "2"), "0", "sim26: NpIslMode(): 1 the islands of a step on the job lanes, the geom notifications and tag resets after the join in the old order (exact), 2 verify"),
    "MSC_AI27_SORT": (["native/platform/compat/np_ai27.cpp"], lambda v: v in ("0", "1", "2"), "0", "ai27: UpdateCachedGameData: a closest-players list the comparator finds in order is not sent to qsort (1), 2 verify (qsort runs on a copy, counts wrong)"),
    "MSC_AI27_VCOPY": (["native/platform/compat/np_ai27.cpp"], lambda v: v in ("0", "1"), "0", "ai27: FuzzyVariantCollection::operator= tests the slot pointers directly instead of calling IsSet twice per slot (1)"),
    "MSC_AI27_AVOID": (["native/platform/compat/np_ai27.cpp"], lambda v: v in ("0", "1", "2"), "0", "ai27: AvoidController::Update asks IsAvoiding once per kind and update (1), 2 verify (the plain call runs as well, counts wrong)"),
    "MSC_AI27_CTRL": (["native/platform/compat/np_ai27.cpp"], lambda v: v in ("0", "1", "2"), "0", "ai27: UserControlledT stops at the first pad assigned to the side (1), 2 verify (GetNumAssignedControllers runs as well, counts wrong)"),
    "MSC_PHYS27_PLANE": (["native/src/Game/Physics/PhysicsWorld.cpp"], lambda v: v in ("1", "2"), "0", "phys27: NpPhys27PlaneMode(): 1 the narrow-phase callback is not run for a pair dCollide surely answers with 0 contacts (no collider for the classes; a plane and a transformed sphere / box / capsule whose AABB is clear of the plane by 0.05; exact), 2 verify"),
    "MSC_PHYS27_DIV": (["native/src/Game/PoseAccumulator.cpp"], lambda v: v in ("1", "2"), "0", "phys27: NpWeightT(): 1 the blend weight w / acc is 1.0f without the divide when nothing was accumulated before (0 + w = w, w / w = 1; exact), 2 verify"),
    "MSC_GT18_QUAT": (["native/src/NL/math.cpp"], lambda v: v in ("0", "1", "2"), "0", "gt18: nlQuatNLerp / nlQuatRenormalize without the calls into the SDK maths (same expressions, same order: exact), and NpWeightT without its shared counters; 2 verify (both ways, a difference counted). In-run flippable (MSC_AB)"),
    "MSC_ODE_BPFULL": (["native/src/ode/collision_space.cpp"], lambda v: v in ("0", "1", "2"), "0", "NpBpFullMode(): 1 the flat broad-phase walk also skips same-body, category / collide and Z rejects, 2 also steps over runs of one body (a character's bone geoms) by the union box of the run; only where unsigned long is 32 bits"),
    "MSC_AI_AVOID": (["native/platform/compat/np_aiavoid.cpp"], lambda v: is_int(v, 1, 7), "0", "NpAiAvoidMode(): bit 1 the pure tests of CanAddAvoidance before the AVL lookup, bit 2 a squared-distance reject before the emulated square root of the base AvoidableObject::IsWithinRange, bit 4 verify (counts wrong rejects)"),
    "MSC_POSE_JOBS": (["native/platform/compat/np_crowdcut.cpp"], lambda v: is_int(v, 0, 3), "0", "NpPoseJobs(): EnvInt 0..3 (wave 12, default 0 in the code, 3 in the Vita plan), bit 1 the crowd sprites' poses (evaluate, node and skin matrices) and view matrices are made on the job lanes before UpdateSprites' capture loop, bit 2 the drawn world NPCs' poses before WorldNPCManager::Render; the same matrices"),
    "MSC_CROWD_GOV": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "0", "CrowdGovOn(): 0..1 (crowd25, default 0 in the code, 1 in the Vita plan): the adaptive crowd-cost governor (np_crowdgov.h): in live play the crowd models' redraw interval grows by size class (far first, six levels; a model's clock only moves when it is redrawn, so a frozen section resumes without a snap) while the game thread is the limiting lane and over 87 % of the period, and comes back when there is room; render only"),
    "MSC_NPC_EVAL_LAZY": (["native/platform/compat/np_crowdcut.cpp"], lambda v: v in ("0", "1"), "0", "NpNpcEvalLazy(): EnvInt 0..1 (default 0 in the code, 1 in the Vita plan): WorldNPCManager::Update does not evaluate the NPC poses, WorldNPCManager::Render evaluates the ones its filter passes"),
    "MSC_CROWD_GT": (["native/platform/compat/np_crowdcut.cpp"], lambda v: is_int(v, 1, 7), "0", "NpCrowdGT(): EnvInt 0..7, bit 1 a crowd sprite's view matrices are made where the sprite is captured, bit 2 the sprites of one texture are captured on the same picture (one pose), bit 4 no view for a captured sprite with no figures"),
    "MSC_PICTURE_HOLD_PACE": (["renderer/src/vgx_backend_gl.cpp", "native/platform/vita/src/vita_plan.cpp"], lambda v: v in ("0", "1"), "0", "Init(): 1 reads GPU time against the pacing's budget (vgx_hold.h Pacing) and shortens the proof of a step given back; the build sets 1 with the hold"),
    "MSC_PICTURE_HOLD_GPUBUSY": (["renderer/src/vgx_backend_gl.cpp", "native/platform/vita/src/vita_plan.cpp"], lambda v: v in ("0", "1"), "0", "Init(): 1 counts a late picture towards a size step only when the GPU's busy time limits the pictures (vgx_hold.h Pacing::GpuGate); the build sets 1 with the hold"),
    "MSC_PICTURE_HOLD_AALAST": (["renderer/src/vgx_backend_gl.cpp", "native/platform/vita/src/vita_plan.cpp"], lambda v: v in ("0", "1"), "0", "Init(): 1 sheds picture size first with the samples kept (Policy::SetOrder), the samples last at the bottom; needs MSC_PICTURE_HOLD_MSAA >= 2; the build sets 1 with the hold"),
    "MSC_HELD_AA_AHEAD": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("0", "1"), "0", "HoldTick: 1 makes the one-sample held-scene target ahead outside play for a stadium whose held scenes are one sample (MSC_PICTURE_HOLD_MSAA_HELD), under the room rule"),
    "MSC_DUMP_START": (["renderer/src/vgx_backend_gl.cpp", "native/platform/vita/src/vita_plan.cpp"], lambda v: v in ("0", "1"), "0", "Init(): 1 (with MSC_PICTURE_HOLD_START) makes the match memory answer the bottom step for The Dump (stadium 4) when nothing was learned (vgx_dumpplay.h Memory::Get), so the first match there starts on 704x400 instead of stepping 9 to 10 in its first seconds; no other stadium; the build sets 1 with the hold"),
    "MSC_PICTURE_HOLD_START": (["renderer/src/vgx_backend_gl.cpp", "native/platform/vita/src/vita_plan.cpp"], lambda v: v in ("0", "1"), "0", "Init(): 1 starts a match at the last match's level here, else the stadium table (vgx_hold.h StartLevel), with 480 frames excused from proof; needs AALAST; the build sets 1 with the hold"),
    "MSC_PICTURE_HOLD_SCENES": (["renderer/src/vgx_backend_gl.cpp", "native/platform/vita/src/vita_plan.cpp"], lambda v: v in ("0", "1"), "0", "Init(): 1 keeps the live level in replays and cut-scenes of a match (HoldSceneStep); needs AALAST; the build sets 1 with the hold"),
    "MSC_PICTURE_HOLD_BUDGET_MARGIN": (["renderer/src/vgx_backend_gl.cpp", "native/platform/vita/src/vita_plan.cpp"], lambda v: 0.5 <= float(v) <= 1.0, "1.0", "Init(): a held 30's budget is this share of its period (Pacing::margin); 1.0 is the whole period; the build sets 0.80 with the hold"),
    "MSC_PICTURE_HOLD_COMMIT": (["renderer/src/vgx_backend_gl.cpp", "native/platform/vita/src/vita_plan.cpp"], lambda v: v in ("0", "1"), "0", "Init(): 1 decides the ladder and samples at kick-off (HoldCommit) and keeps the hold's targets in a match; the build sets 1 with the hold"),
    "MSC_DYNRES_LATEHOLD": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, 1, 30000), "0", "dynres late hold: EnvInt clamped 0..30000 ms, 0 is off; acts at 60 only unless MSC_DYNRES_LATE30"),
    "MSC_FX_COMPOSITE": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v == "1", "0", "FxCompositeOn(): EnvInt != 0 sends the large additive and soft-alpha particle draws to the reduced accumulation target"),
    "MSC_FX_MENU": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("0", "1"), "1", "FxDecide(): EnvInt == 0 keeps menu/title-screen effects (giant ball glow) at full picture resolution instead of the low-res FX compositor"),
    "MSC_PICTURE_HOLD_MSAA_HELD": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("same", "0", "off", "all", "auto") or re.fullmatch(r"\d+(,\d+)*", v) is not None, "same", "HeldAaOneSample(): same (or 2) = replays and cut-scenes keep the live-play samples; 0/off/all = one sample in every stadium; auto = the built-in stadium list; a comma list of stadium numbers"),
    "MSC_AA_POLICY": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("same", "auto"), "same", "AaPolicyFor(): auto reads the per-stadium table {live samples, held samples} (kAaAuto, MSC_AA_TABLE=id:live:held,... overrides); same is no table"),
    "MSC_FX_CELEB_MINPX": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, 1, 2000000), "0", "FxMinPx(): EnvInt, in cut-scene and replay scenes with the compositor on a run must cover at least this many estimated pixels (EFB) to get its own reduced pass; 0 is off"),
    "MSC_MEGA_SHARP": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("1", "2"), "0", "MegaSharpLevel(): EnvInt clamped 0..2 (needs MSC_FX_COMPOSITE); 1 in the Mega minigame, 2 also in the captain's launch and result cut-scenes the effects target has no MSC_FX_LOWRES_MAXH cap and its depth copy takes the nearest depth of a texel, so the ball's edge is not overwritten by the halo"),
    "MSC_MEGA_BALLS": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v == "1", "0", "MegaBallsOn(): EnvInt (needs MSC_MEGA_SHARP >= 1): in the buffers MSC_MEGA_SHARP acts in, the effects pass's depth copy reads the texel centre and the composite does nearest-depth upsampling against the picture's own depth, so the flying Mega Strike balls keep a full-size outline; 0 is the nearest-depth copy of MSC_MEGA_SHARP"),
    "MSC_MEGA_FX_MAXDIV": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, 2, 8), "8", "FxDecide(): EnvInt clamped 2..8 caps the effects target divisor in Mega Strike scenes so effects never collapse to 1/8 resolution"),
    "MSC_BALL_FINS": (["renderer/src/vgx_texcache.cpp", "renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("0", "1", "2"), "1", "BallFinsMode()/Draw(): 1 upscales the 32x32 spike texture to 128x128; 2 adds a second blended rim pass for smooth anti-aliased contours without new shader variants"),
    "MSC_FX_LOWRES_MAXH": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, 64, 544), "0", "FxTargetSize(): EnvInt, 0 is no cap, 64..544 caps the effects target's lines"),
    "MSC_VGX_COPYINPLACE": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v == "1", "0", "copy-in-place scan: EnvInt, 1 samples the EFB colour in place of the shadow-volume copy, 0 copies"),
    "MSC_VGX_DISCARD_BLEND": (["renderer/src/vgx_shadergen.cpp"], lambda v: v in ("0", "1"), "0", "DiscardBlendOn(): EnvInt != 0; 1 makes an alpha-tested draw whose blend keeps a source of 0 (and writes no depth) fragment-key FSK_NODISCARD (vec4(0) instead of discard), pixel-exact by the sgx16 proof"),
    "MSC_VGX_ATLATE": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("1", "2"), "0", "AtlateMode(): EnvInt clamped 0..2, 0 is off; 1 and 2 sink the alpha-tested draws behind the opaque ones of a window"),
    "MSC_LIVE_TRADES": (["native/include/np/livetrades.h", "native/platform/vita/src/vita_plan.cpp"], lambda v: v == "1", "0", "NpLiveTrades(): getenv, empty or a first character 0 is off; the master of the live-play trades (the plan sets 1 with MSC_PICTURE_HOLD on)"),
    "MSC_VGX_ATLASJOIN": (["renderer/src/vgx_draw.cpp"], lambda v: v == "1", "0", "AtlasJoinOn(): EnvInt != 0 puts the crowd, shadow and figure pictures into one texture"),
    "MSC_VGX_SHADOWDOWN": (["renderer/src/vgx_draw.cpp"], lambda v: v == "1", "0", "ShadowDownOn(): EnvInt != 0 (and the join on) draws a shadow picture at half size straight into its cell"),
    "MSC_FX_GLOW_DEAD": (["native/include/np/scenecut.h"], lambda v: v in ("0", "1"), "NpSceneGlowDeadParse(): only 1 is on (the glow chain stops one level early when the composite does not read the last level)"),
    "MSC_HELD_SIZE_DOWN": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("0", "1"), "HoldApply(): 1 draws replays, cut-scenes and walk-outs (the neutral held scenes, not Mega Strikes) at the hold ladder's bottom kept level, 864x488 (the Quality floor) with one sample; needs AALAST and the floors; live play unchanged"),
    "MSC_HELD_GOV_SIZE": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("0", "3", "4", "5"), "the held-scene governor's level 4 (scenes30, vgx_heldgov.h): ladder rung below the Quality floor (3 = 800x456); 0 keeps the 864x488 Quality floor (int26b default: prevents Q30 6405/6406 CDRAM overflow and 47-75 RT destroys)"),
    "MSC_HELD_GOV": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("0", "1"), "HeldGovOn() (scenes30, vgx_heldgov.h): 1 the held-scene GPU governor: only in a held cut-scene or replay whose GPU time is over 33.3 ms, crowd floor +1, shadows +1 picture, glow every 3rd picture; 0 off"),
    "MSC_FX_GOV_HOLD": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v.isdigit() and 0 <= int(v) <= 600, "FxGovHold() (scenes30, vgx_fxgov.h absentHold): frames the fill governor keeps its thinning through frames without large sprites; 0 lets go at once as before"),
    "MSC_VGX_FEPAR_KEEP": (["renderer/src/vgx_fepar.inc"], lambda v: v.isdigit() and 0 <= int(v) <= 7, "wave 25: the front end's decode worker kept on core 3; every mode since 26 Nov (owner 08:28: exact cuts apply to Quality too)"),
    "MSC_FPS_PROBE_ASYNC": (["native/platform/vita/src/np_fps_probe.cpp"], lambda v: v in ("0", "1"), "wave 25: the per-core probe read off the game thread; every mode since 26 Nov (owner 08:28: exact cuts apply to Quality too)"),
    "MSC_VGX_RT_POOL": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, 0, 48), "0", "int26 (console 6251): pools SceGxmRenderTarget driver objects in LPDDR (0 KiB CDRAM, 8 MiB cap), cutting RT destroys 26->2 in Smooth 60 and 70-148->near 0 in Quality 30"),
    "MSC_VITA_FBO_KEEP": (["renderer/vita/vgx_host_vita.cpp"], lambda v: v in ("0", "1"), "0", "int26/int26b (console 6250/6405): vitaGL 0062 keeps FBO RT + depth across detach; code default 0, Vita plan 1 in kQuality (RT destroys -> 1, -6 MiB peak CDRAM)"),
    "MSC_FX_OFFVIEW": (["native/src/Game/Effects/ParticleSystem.cpp"], lambda v: is_int(v, 0, 3), "0", "int26: exact off-screen sprite culling before vertex write"),
    "MSC_STADIUM_CUT_FAR": (["native/src/Game/Render/StadiumCut.cpp"], lambda v: is_float(v, 1.0), "24", "NpStadiumCutFarParse(): atof clamped 1..200, pixel size under which NP_CUT_FARDECOR drops a drawable (default 24; Vita plan 32)"),
    "MSC_HELD_SCENES": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, 1, 7), "0", "heldscenes::ParseBits(): bit 1 depth read-scan, 2 pass memo + run merge, 4 half-scale blur passes, in held scenes only"),
    "MSC_FX_GLOW_COARSE": (["native/include/np/scenecut.h"], lambda v: v == "1", "0", "NpSceneGlowCoarseParse(): 1 runs the glow chain on the game's 4-pass coarse route"),
    "MSC_FX_GLOW": (["native/include/np/scenecut.h"], lambda v: v == "half", "full", "NpSceneGlowHalfParse(): half stops the glow chain at 160x112; full is the full chain"),
    "MSC_FX_GOV_MAX": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("1", "2"), "2", "DynInit(): 1 caps fill-governor thinning at 256 (1-in-2 at 2x alpha), avoiding 4x strobing during walk-out/DK fly-in"),
    "MSC_LANE_CORE3": (["native/platform/vita/src/np_framesend.cpp"], lambda v: v in ("0", "1"), "0", "int28 (console 6420): moves blend helper lane to Core 3 (online-guard registered), cutting Core 0 takeovers 73.1% -> 2.1%"),
    "MSC_VITA_PRIO_LANE": (["native/platform/vita/src/np_framesend.cpp"], lambda v: re.fullmatch(r"0x[0-9A-Fa-f]+", v) is not None, "0", "int28 (console 6420): blend helper lane thread priority on Core 3"),
    "MSC_HELD_GLOW_HALF": (["native/include/np/scenecut.h"], lambda v: v in ("0", "1", "2"), "NpSceneGlowRateParse(): 0 off, 1 the glow chain of a held scene (replay, cut-scene, walk-out) is redrawn every other picture, 2 every third; the first 6 pictures of a held scene are always drawn"),
    "MSC_VGX_SHQUAD_LIVE": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("0", "1", "2"), "0", "ShqScan(): EnvInt, 0 skips live flat-shadow play, 1 scans all stadiums, 2 scans heavy stadiums only (int26b default: saves 1.39 ms CPU / 4.03 ms wall on Core 1 in Vice live play, exact)"),
    "MSC_VGX_LEDGERPAL": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("0", "1"), "0", "LedgerPalOn(): EnvInt, 1 lets the alpha ledger measure draws through the position-matrix palette (the flat shadow ellipses); needs MSC_VGX_PALIDX=1"),
    "MSC_VGX_SHQUAD": (["renderer/src/vgx_draw.cpp", "renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, 1, 2), "0", "FillBoxesWanted() / ShqMode(): EnvInt, 0 is off, 1 rewrites the quad, 2 scissors it"),
    "MSC_VITA_QUEUE_FIXED30": (["renderer/vita/vgx_host_vita.cpp"], lambda v: v in ("1", "2"), "0", "LimitGpuQueue(): atoi, >= 2 is mode 2, 1 is mode 1, anything else off"),
    "MSC_VITA_QUEUE_FIXED60": (["renderer/vita/vgx_host_vita.cpp"], lambda v: v in ("1", "2"), "0", "int29 (console 6431/6442-6447): LimitGpuQueue() depth 2 held at 60 fps unless GPU > 105% (17.5 ms), preventing 13.0 ms queue collapse"),
    "MSC_HOLD60_WALLSIM": (["native/platform/compat/np_fps.cpp"], lambda v: v in ("0", "1"), "0", "int30 (wallsim25_unit + console 6454): prices 2-step simulation wall time at 30 fps in the frame clock so Vita runClocks staleness (~5.4 ms) does not trap Vice at 30 fps"),
    "MSC_VITA_STREAM_CACHED": (["renderer/vita/vgx_host_vita.cpp"], lambda v: v in ("0", "1"), "0", "int29 (console 6446/6447): vitaGL 0060 cached stream arena flushed once per scene submit (1.0 write-back/pic, 0 spills, -2 ms BE)"),
    "MSC_VGX_BEPREFETCH": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, 0, 8), "0", "int29 (console 6446/6447): ARM pld prefetch N draws ahead on Core 1 BE (hint only, exact)"),
    "MSC_SKIN_POSE_PAR": (["native/src/Game/Drawable/NpSkinPosePar.cpp"], lambda v: v in ("0", "1", "2"), "0", "int29 (console 6446/6447): exact parallel character skin-mesh posing on Core 0+3 (0 mismatches, -1.6 to -2.1 ms Core 0)"),
    "MSC_SKIN_POSE_PAR_AVOID": (["native/src/Game/Drawable/NpSkinPosePar.cpp"], lambda v: re.fullmatch(r"[0-3]+", v) is not None, "", "int29 (console 6446/6447): avoid waking Core 1 BE and Core 2 FE for skin-mesh pose jobs (12 = cores 1 & 2)"),
    "MSC_VGX_LA88": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("0", "1", "2", "3"), "0", "int29 (console 6447): exact 1-byte I4/I8 (GL_INTENSITY) and 2-byte IA4/IA8 uploads (-1.4 to -5 MiB CDRAM)"),
    "MSC_VGX_HSR_PROVEN": (["renderer/src/vgx_shadergen.cpp"], lambda v: v in ("0", "1"), "0", "int29: exact draw-level alpha=1 interval proof restoring SGX TBDR hidden-surface removal"),
    "MSC_VGX_ZERODRAW": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("0", "1"), "0", "int29: exact draw-level provably-no-op blended draw cull (never skips depth writes)"),
    "MSC_VITA_PRIO_VBLANK": (["native/platform/sdk/vita/np_plat_vita.cpp"], lambda v: re.fullmatch(r"0x[0-9A-Fa-f]+", v) is not None, "0", "apply_role(): strtoul base 0; 0 or off keeps the timer role's priority"),
    "MSC_VITA_CORE0_ALONE": (["native/platform/vita/src/vita_threads.cpp"], lambda v: v == "1", "0", "KeepCoreZeroForGame(): unset, empty, 0 or off is off; log lists; anything else moves"),
    "MSC_PACE_VBLANK": (["native/platform/compat/np_framepace.cpp"], lambda v: v == "1", "0", "NpPaceBeginFrame(): a value not starting with 0 and not 'off' is on"),
    "MSC_SLIP": (["native/platform/compat/np_fps.cpp"], lambda v: is_int(v, 1, 25), "0", "NpSlipDelta(): 'on' is 20, else atoi clamped to 0..25; 0 is off"),
    "MSC_DYNRES_UNDO": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v == "1", "0", "DynInit(): EnvInt, 0 is off, anything else judges every step down in an ordinary scene on its own frames and undoes one that saved no GPU time"),
    "MSC_DYNRES_SEVERE": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v == "1", "0", "DynInit(): EnvInt, 0 is off, anything else opens the deeper floor under a severe overload; a hand-set MSC_DYNRES_MIN keeps it off"),
    "MSC_VGX_CULL": (["renderer/src/vgx_draw.cpp"], lambda v: v == "1", "0", "CullMode(): EnvInt, 0 is off, 1 drops the draws, 2 only counts them"),
    "MSC_CADENCE": (["native/platform/compat/np_fps.cpp"], lambda v: v == "1", "0", "Init(): getenv, 0 off; AUTO only: replays and cut-scenes held at a locked 30 from their cut, open play never held"),
    "MSC_PRESENT_SYNC_START": (["renderer/src/vgx_present.cpp"], lambda v: v == "1", "0", "EnvInt, 0 off: picture alignment starts at 3 vblanks and tightens in 32-picture windows"),
    "MSC_FX_ATTACH": (["native/include/Game/Effects/ParticleSystem.h"], lambda v: v == "1", "0", "NpFxAttachOn(): getenv, 0 off: the live ball trail's particles are drawn at the drawn ball position"),
    "MSC_CROWD_VISFAST": (["native/platform/compat/np_crowdcut.cpp"], lambda v: is_int(v, 1, 2), "0", "NpCrowdVisFast(): EnvInt, 0 the game's loop over every figure, 1 precomputed figure lists per layout and sprite, 2 also compared with the loop"),
    "MSC_CADENCE_FULL": (["native/platform/compat/np_fps.cpp"], lambda v: v == "1", "0", "Init(): getenv, 0 off; AUTO with MSC_CADENCE only: a scene held at 30 is drawn at the game's own size from its cut, the ladder's size back at the cut that ends the hold"),
    "MSC_DL_KEEP": (["renderer/src/vgx_dlcache.cpp"], lambda v: is_int(v, 1, 256), "0", "KeepOn(): EnvInt MiB, 0 off: display-list entries unused for 600 frames are kept while the due ones add up to this many MiB (oldest go first, 24 a sweep)"),
    "MSC_VGX_SKINDIRECT": (["renderer/src/vgx_state.cpp"], lambda v: v == "1", "0", "SkinDirect(): EnvInt, 0 expands skinned vertices at the unskinned stride then copies, 1 decodes at the final stride"),
    "MSC_VGX_SKINMEMO": (["renderer/src/vgx_state.cpp"], lambda v: is_int(v, 1, 2), "0", "SkinMemoMode(): EnvInt, 0 off, 1 skin rows kept per weight array, 2 also compared with the long way (host)"),
    "MSC_VGX_BUILDFAST": (["renderer/src/vgx_dlcache.cpp"], lambda v: v == "1", "0", "BuildFast(): EnvInt, 0 off, 1 one allocation per entry build and a one-load box"),
    "MSC_VGX_EXTMEMO": (["renderer/src/vgx_draw.cpp"], lambda v: is_int(v, 0, 2), "1", "ExtMemoMode(): EnvInt, 0 TevRegsWide() and LightFactId() computed on every call, 1 kept while g_extGen has not moved, 2 also computed the long way and compared (host)"),
    "MSC_VGX_DLWATCH": (["renderer/src/vgx_dlcache.cpp"], lambda v: is_int(v, 0, 2), "1", "DlWatchMode(): EnvInt, 0 every display-list hit reads its page generations, 1 a hit whose generations were read at the current watch epoch (writes to watched pages only) is valid without reading them, 2 also read and compared (host)"),
    "MSC_VGX_LEDGERFAST": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, 0, 2), "1", "LedgerFastMode(): EnvInt, 0 the shadow quad's alpha ledger projects every triangle corner and walks every polygon, 1 each vertex is projected once per draw, a polygon the bands already hold is skipped and the band-cost table is made once, 2 also the plain form, compared (host)"),
    "MSC_VGX_CROWDTRIM_RECT": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, 0, 2), "0", "CtRectMode(): EnvInt, 0 a crowd-trim cell set tests every cell for overlap, 1 a set with the rectangle of a live cell takes that cell's slot (exact by disjointness; febe24 host proof; Vita plan 1), 2 also the plain form on a copy, compared"),
    "MSC_VGX_CROWDTRIM_WELD": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, 0, 1), "0", "CtWeldOn(): VGX_KNOB(EnvInt), 1 a static figure draw of the crowd trim is measured over the vertices its indices name (welded lists were refused; gpueff18, exact; Vita plan 1)"),
    "MSC_VGX_CROWDTRIM_SKEW": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, 0, 1), "0", "CtSkewOk(): VGX_KNOB(EnvInt), 1 slightly bent crowd quads (held scenes) are cut too, corners on the original triangles (gpueff18, isolated pixels; Vita plan 1)"),
    "MSC_VGX_CROWDTRIM_PROJ": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, 0, 1), "0", "crowd trim quad pass: EnvInt, 1 a q = 1 projective texgen is taken as plain (Vice's held crowd; wave 7, plan 1 since gpueff18)"),
    "MSC_VGX_VGL_MASKBG": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, 0, 3), "1", "DrawCachesInit(): VGX_KNOB(EnvInt), vitaGL 0029 vglSetMaskBackground: bit 0 framebuffer-object scenes, bit 1 the display's scene (the present) begin with the mask set and the full-target mask quad is left out; model test run_vitagl_maskkeep_test.sh; Vita plan 3 (gpu24; code default 1)"),
    "MSC_VITA_WRCOPY": (["renderer/vita/vgx_host_vita.cpp"], lambda v: v in ("0", "1", "2", "3", "auto", "bench"), "0", "WriteCopySetup(): getenv, copies into the uncached pools: 0 plain, 1/2 NEON stores, 3 DMA, auto the fastest byte-exact variant of a start-up benchmark (be24); Vita plan auto (wave 24)"),
    "MSC_VGX_CROWDTRIM_PLAY": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, 0, 1), "1", "CtPlayOff(): EnvInt, 1 the crowd trim follows the impostor passes in every picture, 0 it does not in match-play pictures (buf.scene 1; the Vita plan sets 0): cells are dropped, the crowd is drawn whole in open play, the trim still acts in walk-outs, replays and cut-scenes"),
    "MSC_REC_FAST": (["renderer/src/vgx_rec.cpp"], lambda v: is_int(v, 0, 1), "1", "Reset(): EnvInt into g_recFast, 0 the recorder's register writes and fog set-up as before, 1 the inline / early-out / memo forms (same stream bytes)"),
    "MSC_VGX_FASTIDX": (["renderer/src/vgx_state.cpp"], lambda v: v == "1", "0", "FastIdx(): EnvInt, 0 guarded 16-bit index kernels, 1 range-proven faster kernels"),
    "MSC_VGX_VGL_FRAGMEMO": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v == "1", "0", "DrawCachesInit(): EnvInt, 0 is off, anything else on"),
    "MSC_VGX_VGL_UNIREUSE": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v == "1", "0", "DrawCachesInit(): EnvInt, 0 is off, anything else on"),
    "MSC_VGX_VGL_UNIHIST": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, 0, 2), "0", "DrawCachesInit(): EnvInt, needs UNIREUSE; 0 only the last copy is named again, 1 also any of the last four copies of the scene (vitaGL 0046), 2 as 1 and every reuse is read back and compared (test mode)"),
    "MSC_FPS_LADDERHOLD": (["native/platform/compat/np_fps.cpp"], lambda v: v == "1", "0", "FpsConfigFromEnv: atoi != 0 holds AUTO at 60 on a GPU-limited miss while the dynamic resolution has picture steps left; 0 is the old rules"),
    "MSC_VGX_PASSDOWN_TWOQUADS": (["renderer/src/vgx_draw.cpp"], lambda v: v == "1", "0", "DownPasses(): EnvInt, 1 lets a glow step of two quads (12 indices; The Dump and Sand Tomb draw each step twice) go straight at half scale as well, 0 only one quad (6 indices)"),
    "MSC_VGX_PASSDOWN": (["renderer/src/vgx_draw.cpp"], lambda v: v == "1", "0", "DownPasses(): EnvInt, 0 is off, 1 draws the alpha-only glow passes straight at half scale"),
    "MSC_DISCARD_FAST": (["native/src/Game/InputManager.cpp"], lambda v: v == "1", "0", "NpDiscardFastAllowed(): atoi, 0 or unset is off (a discarded frame waits for one retrace, as the original game); not in a LAN / online / recorded game"),
    "MSC_PIC_PREWARM": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v == "1", "0", "PrewarmPicturePrograms(): EnvInt, 0 is off (the picture program of a mode is built at its first present), anything else builds them at start-up"),
    "MSC_POOL_KEEP": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, 601), "0", "PoolKeepPresents(): EnvInt, presents an unused pass object is kept while the video memory is roomy; 0 or less is the 600 of before"),
    "MSC_VGX_HOUSEKEEP_BACKOFF": (["renderer/src/vgx_backend_gl.cpp", "renderer/src/vgx_texcache.cpp", "renderer/src/vgx_dlcache.cpp", "renderer/src/vgx_draw.cpp"], lambda v: v == "1", "0", "HousekeepBackoff() / TexCacheEndFrame() / the list-cache, soft-instance and palette-merge sweeps: EnvInt != 0 makes a program or texture trim pass that deleted nothing wait 30 pictures / 4 frames, and a 120-frame sweep that freed nothing wait up to 480, before it scans again; 0 scans at the old pace"),
    "MSC_POOL_THRASH_GUARD": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v == "1", "0", "ThrashGuardOn(): EnvInt != 0 pins a pass-object or kept-target size that was made again within 3600 presents of being trimmed (TrimPool leaves it); 0 trims by age as before"),
    "MSC_VGX_WARM_ORDER": (["renderer/src/vgx_draw.cpp"], lambda v: v == "1", "0", "WarmOrderOn(): EnvInt != 0 marks (list record 5) the programs drawn in a match and links the marked ones first at the next start; 0 keeps the list in file order and writes no marks"),
    "MSC_WARM_MATCH": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v == "1", "0", "WarmMatchOn(): EnvInt != 0 makes in picture 1 only the first MSC_WARM_CAP (400; 600 in the Vita plan, raised to cover marked match programs up to ProgramLimit()-64) programs of the list with their shaders and variants; the rest of the list is left to first use; 0 makes the whole list in picture 1 (held only by the heap rule)"),
    "MSC_WARM_CAP": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, 0, 1024), "400", "WarmPlanBuffer(): maximum base programs linked during picture-1 boot warm-up (600 in the Vita plan; MSC_WARM_CAP_MARKED=1 raises dynamically to cover marked match programs, unconditionally clamped to ProgramLimit()-64 = 836 on Vita)"),
    "MSC_WARM_BLOCK": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v == "1", "0", "WarmBlockOn(): EnvInt != 0 makes a draw that is not a plain main-scene colour draw (pass, copy source, depth-only) compile and link its program at once instead of being left out under the one-compile-a-frame rule; 0 leaves such a draw out as before"),
    "MSC_SLIP_STEPS": (["native/platform/compat/np_fps.cpp"], lambda v: is_int(v, 1, 5), "0", "NpSlipDelta(): atoi clamped 0..5 (needs MSC_SLIP on); a late frame advances the game by at most this many 20 ms steps (never less than the slip limit of one period); 0 is off"),
    "MSC_SLIP_REFILL_PCT": (["native/platform/compat/np_fps.cpp"], lambda v: re.fullmatch(r"[0-9]+(\.[0-9]+)?", v) is not None and 0 <= float(v) <= 20, "2", "SlipInit(): atof clamped 0..20 (percent of real time the slip bucket fills at)"),
    "MSC_SLIP_BUDGET_MS": (["native/platform/compat/np_fps.cpp"], lambda v: is_int(v, 1, 200), "0", "SlipInit(): atof clamped 0..200 ms (needs MSC_SLIP on); the slip's bucket: a late frame is cut only from it, it fills at MSC_SLIP_REFILL_PCT (2) of real time; 0 is the stateless rule"),
    "MSC_VITA_SWEEP_BG": (["native/platform/vita/src/vita_threads.cpp"], lambda v: v == "1", "0", "SweepInBackground(): '1' only; the 600-frame thread sweeps (KeepCoreZeroForGame, PinLibraryThreads) run on the watch thread"),
    "MSC_LAT_DEPTH_PACED": (["native/platform/compat/np_fps.cpp"], lambda v: is_int(v, 0, 3), "1", "pacedExtra: atoi clamped 0..3; the frames in flight a free-running pace keeps beyond MSC_LAT_DEPTH (code 1, plan 2)"),
    "MSC_FPS_BETWEEN_ENTER": (["native/platform/compat/np_fps.cpp"], lambda v: is_int(v, 5, 60), "26.7", "Init(): EnvDouble ms; the busiest stage under it for a second lets the free run in (code 26.7, plan 20)"),
    "MSC_FPS_BETWEEN_LEAVE": (["native/platform/compat/np_fps.cpp"], lambda v: is_int(v, 5, 60), "30", "Init(): EnvDouble ms, raised to the enter value; over it for MSC_FPS_BETWEEN_OUT frames ends the free run (code 30, plan 25)"),
    "MSC_FPS_BETWEEN_COOL": (["native/platform/compat/np_fps.cpp"], lambda v: is_int(v, 0, 600), "6", "Init(): EnvDouble s, the wait after a way out (doubles for a try that failed; code 6, plan 45)"),
    "MSC_FPS_BETWEEN_COOLMAX": (["native/platform/compat/np_fps.cpp"], lambda v: is_int(v, 0, 3600), "96", "Init(): EnvDouble s, at least the cool-down: the longest wait (code 96, plan 240)"),
    "MSC_FPS_BETWEEN_FAIL": (["native/platform/compat/np_fps.cpp"], lambda v: is_int(v, 0, 3600), "30", "Init(): EnvDouble s: a free run that ended within this long was a failed try (code 30, plan 90)"),
    "MSC_FPS_BETWEEN_OUT": (["native/platform/compat/np_fps.cpp"], lambda v: is_int(v, 1, 600), "20", "Init(): frames over the leave limit that end the free run (code 20, plan 60)"),
    "MSC_DYNRES_PACED_UP_MS": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, 1, 40), "0", "the dynamic-resolution ladder's budget while the rate is free-running (interval 0), atof clamped 0..40; 0 = the 30 budget (vgx_upbudget.h)"),
    "MSC_SMOOTH_CLOCK": (["native/platform/compat/np_smooth.cpp"], lambda v: v in ("0", "1"), "0", "NpSmoothFrameBegin: EnvInt != 0; under a held cadence in live single-player play a frame advances the game by its picture's predicted show-to-show time (np_show_clock.h), after 8 evenly held pictures"),
    "MSC_SMOOTH_LOG": (["native/platform/compat/np_smooth.cpp"], lambda v: v in ("0", "1"), "0", "LogOn(): EnvInt != 0; the [smooth] line (motion against hold per layer, blend histogram, clock counters), a diagnostic that changes no picture"),
    "MSC_TAP_CARRY": (["native/platform/vita/src/np_vita_input.cpp"], lambda v: v == "1", "0", "TapCarryOn(): nonzero atoi; a tap latched by a frame without a step reaches the next step (single player)"),
    "MSC_LAT_DRAIN": (["native/platform/compat/np_fps.cpp"], lambda v: v == "1", "0", "FramesInFlightGate: nonzero atoi; a held 30 standing aside still drains a backlog above depth + 1"),
    "MSC_PAD_LATE": (["native/platform/compat/np_fps.cpp"], lambda v: v == "1", "0", "NpFpsPadLateWait: atoi > 0; the late latch under a held cadence, single player"),
    "MSC_PACE_LOG": (["renderer/vita/vgx_host_vita.cpp"], lambda v: is_int(v, 1, 3600), "0", "g_paceWindowMs: atof seconds, 0 is off; the [pace] render and display lines (vgx_pacelog.h), a diagnostic that changes no picture"),
    "MSC_POOL_TRIM_ONE": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v == "1", "0", "PoolTrimOne(): EnvInt, 0 is off (every due object let go at once), anything else one per present while the video memory is roomy"),
    "MSC_SHARPEN_EASE": (["renderer/vita/vgx_host_vita.cpp"], lambda v: is_int(v, 1), "0", "SharpEase(): atol, pictures over which a size step moves the display pass's sharpening; 0 or unset is the switch in one picture"),
    "MSC_TEX_PRETOUCH": (["native/src/NL/glx/glxTexture.cpp"], lambda v: is_int(v, 0, 2), "0", "NpTexPretouchStep(): 0 off, 1 each rendered frame outside the front end up to MSC_TEX_PRETOUCH_N textures registered with the texture manager are uploaded by the renderer ahead of their first draw (np_gx_touch_tex, EXT_TOUCH_TEX), 2 the same and the textures are kept against the renderer's texture budget until the front end is back"),
    "MSC_DVD_KEEP_KIB": (["native/platform/sdk/src/dvd_native.cpp"], lambda v: is_int(v, 1, 65536), "0", "switches_init(): strtoul KiB (at most 65536), 0 or unset is off: whole .nis/.sanim/.fen files up to MSC_DVD_KEEP_FILE_KIB are read ahead (NpDvdKeepHint) or after a miss and answered from memory"),
    "MSC_HDTEX_FADE": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, 1), "0", "Init(): EnvInt, pictures over which an HD replacement that arrived late eases its mip bias from one level coarser to 0; 0 is off"),
    "MSC_STILL_TOL": (["native/src/Game/Render/StillBackdrop.cpp"], lambda v: v == "1", "0", "StillTolOn(): NpStillParse, only 1 is on; the camera's stamp ignores float noise (matters only with STILL BACKDROPS on)"),
    "MSC_HITCH_MS": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, 1), "0", "HitchLedger(): EnvInt, ms; 0 is off (a diagnostic: [hitch] lines and a summary every 1800 frames)"),
    "MSC_VGX_SOFTXFORM": (["renderer/src/vgx_draw.cpp"], lambda v: is_int(v, 1), "128", "SoftXformLimit(): EnvInt, the largest mesh transformed on the CPU; 0 would be no CPU transform at all, 128 is the 1.1 value"),
    "MSC_VGX_SHQUAD_HELD": (["renderer/src/vgx_backend_gl.cpp"], lambda v: is_int(v, 0, 2), "1", "heldgpu24: EnvInt 0..2; 0 skips shadow-quad CPU scan in held scenes (exact, console 6247 -6.5 ms/pic Core 1)"),
    "MSC_HELD_GOV_SEED": (["renderer/src/vgx_backend_gl.cpp"], lambda v: v in ("0", "1"), "0", "heldcpu24: EnvInt != 0; first replay starts at the cut-scene learned level"),
    "MSC_REPLAY_FARLOD": (["native/include/np/scenecut.h"], lambda v: is_float(v, 0.01), "0", "NpSceneFarLodParse: distance cutoff for match-LOD characters in replays/cut-scenes (int28: 18 avoids DK fly-in body LOD pop)"),
}
# defaults the owner left OFF on purpose: they must not be in vita_plan.cpp's list
KEPT_OFF = ["MSC_FS_HELD", "MSC_VGX_GXMCACHE", "MSC_VGX_FBO_SAMPLES", "MSC_REDUCED_MOTION", "MSC_LAN_INPUT_DELAY",
            # 1.3 (reports/defaults.md): a console-only risk or a shader form outside the 1.3 pack
            "MSC_VGX_TEXSWIZZLE", "MSC_VITA_USSE_CDRAM", "MSC_VGX_PALBUCKET", "MSC_VGX_FS_HALF", "MSC_VGX_FOGVTX", "MSC_VITA_CORE_GC",
            "MSC_FPS_PROBE_EVERY", "MSC_AI_NOCLOCK_BREAK", "MSC_CPU22_BREAK", "MSC_CPU24_BREAK",
            # pace10: console A/B first (np_fps_phase.h); the plan keeps MSC_PAD_LATE=1 until the console decides
            "MSC_PHASE_SYNC",
            # r15: hold12 scene policy, console A/B first (w8/fable-verify-1114.md)
            "MSC_PICTURE_HOLD_SCENEPOL",
            # wave 11 popin16: blocking links in the walk-out trade a hole for a longer picture; console run first
            "MSC_POPIN_BLOCK",
            # helper f45: 45 FPS is a FRAME RATE choice (MSC_FPS=45), never the plan's default until the console says so
            "MSC_CADENCE45"]
# keys a row already writes whose reader is another worker's code, not merged yet: the value is still checked, the reader file is not (the entry says who owns it)
PENDING = {}
# keys an option owns without writing them (older builds' rows): they must still have a reader or the legacy handling is pointless
# (set18: the ANTI-ALIASING row that owned MSC_VITA_MSAA went; np_settings_guard.cpp still turns a 1.0 / 1.1 line off)
LEGACY = {}

# 1.3 prune: keys whose row or choice went from the screen (the owner's "remove any settings that don't make sense anymore or aren't needed", 15 Nov). The reader stays, the kit
# and hand files still set the key (READERS above keeps every entry); the screen no longer writes it. key: the row (or choice) it belonged to. Must equal kRetiredKeys.
RETIRED = {
    "MSC_FPS_BETWEEN": "FRAME RATE: UP TO 60", "MSC_FPS_REPLAY": "REPLAYS & CUT-SCENES", "MSC_PICTURE_HOLD": "FULL PICTURE", "MSC_PICTURE_HOLD_MSAA": "ANTI-ALIASING: 4X",
    "MSC_DYNRES": "DYNAMIC RESOLUTION", "MSC_DYNRES_MAX": "SHARPER WHEN POSSIBLE", "MSC_DYNRES_FG": "PRIORITIZE PLAYERS", "MSC_DYNRES_MIN60": "SMALLEST PICTURE AT 60",
    "MSC_FX_LOWRES": "EFFECTS QUALITY", "MSC_FX_LOWRES_SCENES": "EFFECTS QUALITY", "MSC_FX_LOWRES_MINPX": "EFFECTS QUALITY", "MSC_VITA_ANISO": "DISTANT TEXTURES",
    "MSC_FRAME_PACING": "FRAME PACING", "MSC_FX_GLOW": "GLOW", "MSC_SHADOW_EVERY": "REPLAY SHADOWS", "MSC_REPLAY_FARLOD": "REPLAY FAR PLAYERS", "MSC_VGX_GXM_DIRECT": "GXM DIRECT",
    "MSC_VITA_CORE3": "FOURTH CORE", "MSC_VITA_CORE3_SEND": "FOURTH CORE", "MSC_VGX_LODBIAS": "TEXTURE DETAIL", "MSC_VITA_SHAPE": "PICTURE SHAPE", "MSC_VGX_HUDNATIVE": "NATIVE HUD",
    "MSC_MENU_STILL": "STILL BACKDROPS", "MSC_PAUSE_STILL": "STILL BACKDROPS", "MSC_FX_GOV": "EFFECTS UNDER LOAD", "MSC_FX_GOV_MEGA": "EFFECTS UNDER LOAD", "MSC_SHADOW_PLAY": "MATCH SHADOWS",
    "MSC_VGX_LA88": "TEXTURE MEMORY", "MSC_VGX_CMPRA": "TEXTURE MEMORY", "MSC_VGX_TEX16": "TEXTURE MEMORY", "NP_MENU_NAV": "MENU STICK",
    "MSC_VITA_SHARPEN": "SHARPNESS: CRISP+", "MSC_VITA_EDGE_AA": "SHARPNESS: SMOOTH NO EDGE AA",
    # set18 (owner, 23 Nov: "Remove any settings that aren't needed or are just pure negative to the experience. Simplify where able")
    "MSC_VITA_RENDER": "RESOLUTION", "MSC_VGX_MSAA": "ANTI-ALIASING", "MSC_LAN_INPUT_DELAY": "INPUT DELAY", "NP_TOUCH_POINTER": "TOUCH POINTER",
    "MSC_LAN_MODE": "MULTIPLAYER LINK (the game's menu asks the link)",
    "MSC_LAN_DIALOG": "NEARBY PAIRING (mp24: removed unless it helps)", "MSC_SHADOW_LIVE": "LIVE SHADOWS (owner 20:36: removed unless it helps)", "NP_WIDESCREEN": "PICTURE FORMAT (owner 20:36: removed unless it helps)",
}

failures = []


def fail(message):
    failures.append(message)
    print("FAIL", message)


def reads(path, key):
    text = open(os.path.join(ROOT, path), errors="replace").read()
    for line in text.splitlines():
        if '"' + key + '"' in line and re.search(r"getenv|EnvInt|EnvDouble|EnvOff|EnvOne|env_int|env_flag|EnvF|Has\(|names\[", line):
            return True
    # np_touch.cpp reads the three minigame keys through a name table on the line above getenv
    return any('"' + key + '"' in line and "," in line and "const char* names" in line for line in text.splitlines())


def main():
    with tempfile.TemporaryDirectory(prefix="settings-audit-") as tmp:
        dump = os.path.join(tmp, "settings_dump")
        cmd = ["c++", "-std=c++17", "-I" + SRC, os.path.join(TESTS, "settings_dump.cpp"), os.path.join(SRC, "np_settings.cpp"), os.path.join(SRC, "vita_plan.cpp"), "-o", dump]
        if sys.platform == "darwin":
            cmd.append("-Wl,-undefined,dynamic_lookup")
        subprocess.run(cmd, check=True)
        rows = subprocess.run([dump, "rows"], check=True, capture_output=True, text=True).stdout.splitlines()
        seen = {}
        count = 0
        for line in rows:
            row, idx, label, assigns = line.split("|", 3)
            for item in assigns.split(" "):
                if not item:
                    continue
                key, _, value = item.partition("=")
                count += 1
                if key in LEGACY:
                    fail("%s / %s writes %s: %s" % (row, label, key, LEGACY[key][1]))
                if key in PENDING:
                    seen[key] = True
                    if not PENDING[key][0](value):
                        fail("%s / %s writes %s=%s: not the value its future reader takes" % (row, label, key, value))
                    continue
                if key not in READERS:
                    fail("%s / %s writes %s=%s: no reader registered in the audit (look it up and add it)" % (row, label, key, value))
                    continue
                files, check, what = READERS[key]
                if key not in seen:
                    seen[key] = True
                    for f in files:
                        if not os.path.exists(os.path.join(ROOT, f)):
                            fail("%s: reader file %s is missing" % (key, f))
                        elif not reads(f, key):
                            fail("%s: %s does not read it (no getenv / EnvInt of the literal): a key nobody reads" % (key, f))
                if not check(value):
                    fail("%s / %s writes %s=%s: the reader (%s) does not accept that value" % (row, label, key, value, what))
        for key, (files, what) in LEGACY.items():
            for f in files:
                if not reads(f, key):
                    fail("legacy key %s: %s no longer reads it" % (key, f))
        # 1.3 prune: the retired keys. kRetiredKeys (np_settings.cpp) and RETIRED here are the same set; no remaining choice writes one; every one still has an audit entry and its reader
        src = open(os.path.join(SRC, "np_settings.cpp")).read()
        block = src[src.index("constexpr RetiredKey kRetiredKeys[]"):]
        block = block[:block.index("};")]
        listed = set(re.findall(r'\{"([A-Z0-9_]+)", (?:nullptr|"[A-Z ]+")\}', block))
        for key in sorted(listed - set(RETIRED)):
            fail("kRetiredKeys lists %s but the audit's RETIRED does not (name the row it belonged to)" % key)
        for key in sorted(set(RETIRED) - listed):
            fail("RETIRED lists %s but kRetiredKeys in np_settings.cpp does not" % key)
        written = set()
        for line in rows:
            written |= set(item.partition("=")[0] for item in line.split("|", 3)[3].split(" ") if item)
        for key in sorted(RETIRED):
            if key in written:
                fail("%s is retired (%s) but a remaining choice writes it" % (key, RETIRED[key]))
            if key not in READERS:
                fail("%s is retired (%s) but has no READERS entry: its reader was looked up when the row existed, keep the entry" % (key, RETIRED[key]))
                continue
            for f in READERS[key][0]:
                if not reads(f, key):
                    fail("retired key %s: %s no longer reads it (a reader was removed with the row; the prune keeps every reader)" % (key, f))
        print("audit: %d retired keys, all with readers, none written by a row" % len(RETIRED))
        # set18: kObsolete (values skipped at start-up inside the screen's block) names only keys that still have a reader, and values that no remaining choice writes
        block = src[src.index("constexpr Obsolete kObsolete[]"):]
        block = block[:block.index("};")]
        for key, values in re.findall(r'\{"([A-Z0-9_]+)", "([^"]*)",', block):
            if key not in READERS:
                fail("kObsolete names %s, which has no READERS entry" % key)
            for line in rows:
                for item in line.split("|", 3)[3].split(" "):
                    k, _, v = item.partition("=")
                    if k == key and (values == "*" or v in values.split("|")):
                        fail("kObsolete skips %s=%s but a remaining choice (%s) writes it" % (k, v, line.split("|")[0]))
        # set18: the FRAME RATE row's default follows VitaPlanDefaults::kLock60; the plan's own switch (kLock60Default, vita_plan.cpp) must say the same
        header = open(os.path.join(SRC, "vita_plan_defaults.h")).read()
        row_default = re.search(r"constexpr bool kLock60 = (true|false);", header).group(1)
        plan_src = open(os.path.join(SRC, "vita_plan.cpp")).read()
        m = re.search(r"static const bool kLock60Default = ([^;]+);", plan_src)
        plan_default = m.group(1).strip() if m else None
        if plan_default not in (row_default, "VitaPlanDefaults::kLock60"):
            fail("FRAME RATE row default (vita_plan_defaults.h kLock60 = %s) and vita_plan.cpp kLock60Default (%s) disagree" % (row_default, plan_default))
        # vita_plan.cpp's defaults: every key has a reader, the default is a value the reader takes as ON, the off value is one the reader takes; the kept-off keys are not there
        plan = open(os.path.join(SRC, "vita_plan.cpp")).read()
        block = plan[plan.index("kQuality[][2]"):]
        block = block[:block.index("};")]
        defaults = dict(re.findall(r'\{"([A-Z0-9_]+)", "([^"]*)"\}', block))
        defaults.update(re.findall(r'add\("(MSC_VGX_MSAA|MSC_PICTURE_HOLD|MSC_PICTURE_HOLD_MSAA|MSC_PICTURE_HOLD_PACE|MSC_PICTURE_HOLD_GPUBUSY|MSC_PICTURE_HOLD_COMMIT|MSC_PICTURE_HOLD_AALAST|MSC_PICTURE_HOLD_START|MSC_DUMP_START|MSC_PICTURE_HOLD_SCENES|MSC_PICTURE_HOLD_SCENEPOL|MSC_PICTURE_HOLD_BUDGET_MARGIN|MSC_FX_GOV|MSC_LIVE_TRADES|MSC_SHADOW_LIVE)", "([^"]*)"\)', plan))
        known = dict((k, (v[0], v[1], v[2])) for k, v in DEFAULTED.items())
        for key in ("MSC_DYNRES_FG", "MSC_DOF", "MSC_VITA_ANISO", "MSC_VGX_MSAA", "MSC_PICTURE_HOLD", "MSC_PICTURE_HOLD_MSAA", "MSC_FX_GOV", "MSC_SHADOW_LIVE"):   # (a row writes these: its READERS entry is the check)
            known[key] = (READERS[key][0], (lambda v: v[:1] == "0") if key == "MSC_DOF" else READERS[key][1], None)   # (the build's DOF value is 0 since ship26; the row's ON writes lazy)
        for key, value in defaults.items():
            if key not in known:
                fail("vita_plan.cpp defaults %s=%s: no reader registered in the audit (DEFAULTED)" % (key, value))
                continue
            files, on_ok, _ = known[key]
            for f in files:
                if not reads(f, key):
                    fail("default %s: %s does not read it" % (key, f))
            if not on_ok(value):
                fail("vita_plan.cpp defaults %s=%s: not a value the reader takes as on" % (key, value))
        for key in KEPT_OFF:
            if key in defaults:
                fail("%s must stay off by default but vita_plan.cpp sets it" % key)
        for key in DEFAULTED:
            if key not in defaults:
                fail("DEFAULTED lists %s but vita_plan.cpp does not default it" % key)
        print("audit: %d defaults in vita_plan.cpp checked" % len(defaults))
        print("audit: %d rows' choices, %d assignments, %d keys with readers, %d failures" % (len(rows), count, len(seen), len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
