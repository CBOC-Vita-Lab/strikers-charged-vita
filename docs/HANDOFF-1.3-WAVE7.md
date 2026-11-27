# Strikers 1.3: handoff after wave 7 (12 Nov 2026)

By CBOC-Vita. Start here. This file is for two readers: the owner (plain words, no programming needed for sections 1, 3, 7 and 8)
and the next release engineer (sections 2 to 6 and the pointers at the end).

Ground rules for reading it:

- **Nothing in 1.3 has run on a real Vita.** Every statement below is one of four kinds of evidence, named each time:
  *host pictures* (the computer build drawing frames, compared by pixel), *instruction counts* (the game run on an emulated ARM
  processor, counting work), *emulator* (the whole package run in the Vita3K emulator: it proves the game starts, loads and does not
  fault, it says nothing about speed), or *nothing* (code was written and read, not run).
- **Every millisecond figure is an estimate** and carries its source. There is no frame-rate promise anywhere in 1.3. The numbers
  come only from the console night (section 7).
- Branch `knobs` of the repository is the main line. Head when this was written: `8117769` plus the merge `9fe8a60` (see
  `git log --merges 92f5bc6..knobs`, about 46 merges since 7 Nov).

## 1. State in ten lines

1. The branch `knobs` builds, and the Python tests pass (one expected failure: `test_package_matches_manifest`, which needs a release package).
2. The last Vita package build of the head (build "s") never finished. The newest full package that ran in the emulator is older than the
   head: the later merges (compositor tuning, scenes, fe60, rev2r) were checked on the computer only.
3. The game simulates in fixed 20 ms steps and draws pictures between them; online play is lockstep. Nothing merged this week touches
   the simulation, the random numbers or the input record (checked by the instruction-count harness: same trace, same step hashes).
4. The player build sets its defaults in `native/platform/vita/src/vita_plan.cpp`. The computer harness does not run that file, so a
   few switches read as "off" in code and "on" in the plan. This is on purpose; section 2 lists them.
5. A set of new, always-on speed and smoothness work is in (section 2). It was judged by pictures and instruction counts, not by a clock.
6. A set of further ideas is in the code but off, each with one console run that will judge it (section 3).
7. The shader pack is final (pack 16, outside git, section 5). No shader text changed this week.
8. The test night kit (`tools/console-1.2`) is ready and has tests; it has a few known gaps (section 3, end).
9. **The online pair test has not passed on this tree** (section 4, risk 5). Do not call online "checked".
10. The owner wants: smooth, feels like 60, no stutter, no pop-in, full resolution. Nobody can say yet whether 1.3 does that. The night finds out.

## 2. ON by default, new since 7 Nov

"Off with" is the switch that turns it off: put the line in `ux0:data/mscharged/np-player.txt` (or an `env.txt`) on the console, or use the
Settings row where one exists. "Plan" means set in `vita_plan.cpp`; the code fallback in the computer harness is different (listed at the end).
"Checked" names the evidence.

### Effects and picture work

| What it does | Checked | Off with |
| --- | --- | --- |
| **Effects compositor.** Additive and soft effects (glow, rain, haze, Mega Strike streaks) are drawn into one smaller target and composited once. Thin bright draws (lightning arcs) stay full size; detailed textures go to half size; tiny soft textures stay in. | host pictures (exact A/B with the switch off; ring at the ball edge is a 1 px purple fringe, see risk 2); no emulator run of the final rule set; nothing timed | `MSC_FX_COMPOSITE=0` |
| **Glow passes at half scale.** The six blur steps of the glow chain are drawn straight at the copy's size, six scenes fewer in a replay or celebration. | host pictures (the first test was vacuous, a later merge fixed the pass test; pictures need a console look); no timing | `MSC_VGX_PASSDOWN=0` |
| **Final colour clamp removed where the value ranges prove it is not needed** (shader pack 16 holds both forms, so no compile at run time), with the simplifier `TEV_CANON=2`. | compiled shader instruction counts on the console's own compiler: about 26 % fewer weighted instructions with the clamp form removed (estimate, census-weighted); host pictures; nothing on the console GPU | `MSC_VGX_FINCLAMP=0` and `MSC_VGX_TEV_CANON=0` |
| **Fragment and vertex trims** (`FS_TRIM`, `VS_TRIM`, `SCORE_NODEPTH`: no depth test on score digits so they stop vanishing over the pitch). | host pictures, instruction counts of the compiled text | `MSC_VGX_FS_TRIM=0`, `MSC_VGX_VS_TRIM=0`, `MSC_SCORE_NODEPTH=0` |
| **Crowd and shadow picture cuts.** Crowd refreshed every 2nd picture in play (`CROWD_REFRESH=2`), 2 in replays; player shadow pictures every 2nd (`SHADOW_EVERY=2`); atlas-cell clears as one draw (`CELLCLEAR`); crowd quads built directly (`CROWD_QUADFAST`); pose only for visible players (`POSE_CULL`); crowd pose once per pass (`CROWD_POSEONCE`). | host pictures (shadow shape one or two frames old at edges, 0.25 to 0.6 % of pixels in a still comparison); instruction counts for the CPU ones | `MSC_CROWD_REFRESH=1`, `MSC_SHADOW_EVERY=1`, `MSC_VGX_CELLCLEAR=0`, `MSC_CROWD_QUADFAST=0`, `MSC_POSE_CULL=0`, `MSC_CROWD_POSEONCE=0` |
| **Hidden power-ups are skipped** (`MSC_POWERUP_HIDDEN=1`) and only the bones the model uses are posed (`MSC_SKIN_POSE_NEEDED=1`). | instruction counts (same trace); the power-up rule rests on an assumption nobody has tested on a console, risk 3 | `MSC_POWERUP_HIDDEN=0`, `MSC_SKIN_POSE_NEEDED=0` |
| **Picture-side hitch fixes.** Picture programs and the cell-clear program built at start-up (`PIC_PREWARM`); render targets kept between goals (`POOL_KEEP=18000`, `POOL_TRIM_ONE`); the log writer no longer stalls the game thread; slow frames above 25 ms are logged (`HITCH_MS=25`). A failed target allocation frees idle pooled objects and tries again. | emulator for start-up and no faults; host tests of the keep-and-trim logic; nothing timed | `MSC_PIC_PREWARM=0`, `MSC_POOL_KEEP=0`, `MSC_POOL_TRIM_ONE=0`, `MSC_HITCH_MS=0` |
| **Skinned vertices decoded in place, skin rows remembered, faster entry build and index kernels** (`SKINDIRECT`, `SKINMEMO`, `FASTIDX`, `BUILDFAST`, `SKIN_GPU`). | instruction counts and host picture equality (identical) | the matching `MSC_VGX_...=0` |
| **Draw-count cuts that keep the picture** (`CULL`, `UNIDUP`, `GXM_DIRECT`, `SHQUAD`, `SOFTINST`, `BLENDNODISCARD`, `VGL_FRAGMEMO`, `VGL_UNIREUSE`, `ALLOC_MASK`). | mostly host pictures identical or within one shade; emulator for no faults | the matching key `=0` |
| **Native-size HUD** only with LOCKED 60 (`MSC_VGX_HUDNATIVE`, in the LOCKED 60 group). | host pictures; cost and flip need a console | `MSC_VGX_HUDNATIVE=0` |
| **Sharper ground at an angle** (`MSC_VITA_ANISO=2`, floor 128) and **texture detail follows the picture size** (`LODBIAS_RUNG=60`, sharpening by stretch). | host pictures only | `MSC_VITA_ANISO=0`, `MSC_VGX_LODBIAS_RUNG=0` |

### Smoothness and pacing

| What it does | Checked | Off with |
| --- | --- | --- |
| **Frame Rate AUTO uses the speeds between 30 and 60**; the picture-size governor reads GPU busy without the display wait (`MSC_GPU_BUSY_FIX`, vitaGL patch 0030), starts each stadium at the game's own size, does not step down for one slow frame, undoes a step when it was wrong (`DYNRES_UNDO`, `DYNRES_SEVERE`, `LATEHOLD=2000`, `FPS_LADDERHOLD`). | host tests of the picture-size logic; emulator for start-up; nothing timed | `MSC_GPU_BUSY_FIX=0`, `MSC_DYNRES=off`, `MSC_FPS=30` |
| **Waiting banner.** "Waiting for the other player..." with seconds after 1.5 s without the partner's input in a LAN or online match. Display only. (`MSC_WAIT_BANNER`, code default 1; not a plan row.) | host tests (14 checks); compiled on the host only | `MSC_WAIT_BANNER=0` |
| **Allocator aligned with a mask, replay camera remembers its config slots** (`ALLOC_MASK`, `REPLAYCAM_CFG`). | instruction counts: replay stretch 5.9 M down to about 3.7 to 4.4 M instructions a picture (estimate, harness build, divide-by-zero guards inflate the baseline); trace identical | `MSC_ALLOC_MASK=0`, `MSC_REPLAYCAM_CFG=0` |

### Stability, crash handling, text, settings

| What it does | Checked | Off with |
| --- | --- | --- |
| **Every file open and close goes through one lock; a file the start needs is retried up to three seconds** (named on the loading screen). Number formats the Vita's C library lacks (`%zu`, `%zx`, `%lld`, `%j`) swept out; divisions by zero guarded. | host tests; emulator found a real `%zu` fault in a diagnostic line, now fixed | not switchable |
| **Refused vertex growth drops draws instead of writing to null** (`MSC_VGX_ARENA_SOFT`, code default on); framebuffer delete queued once; 16 MiB arenas taken first; heap probe available (`MSC_VITA_HEAPPROBE`). | emulator (96 MiB heap run to the end of one match: 29.8 MiB free in the C heap after match 1); host arena test; the 12-match route was never run (risk 6) | `MSC_VGX_ARENA_SOFT=0` |
| **Settings guard and screen.** A stale or hand-edited settings file is checked; pages are shorter; language follows the system; the version is shown; start-up failures name their cause. Spanish and French for the port's screens and the disc's online screens, 28 more disc messages reworded for the Vita; online messages translated by id. | host runtime tests (136 cases at the last count), audit of keys | `MSC_LANGUAGE=en` for the language only |
| **Online edges.** A partner the online service reports as gone ends the connecting wait; game-start message checked (character and stadium index); stray pause messages ignored; a silent guest is replaced in a room; the server limits requests per address. | host tests (954 online checks, 0 failed); the pair test has NOT passed on this tree (risk 5) | not switchable |
| **Music pauses and resumes when the sound device falls behind** (`MSC_AUDIO_STREAM_KEEP`); audio open retries (`MSC_AUDIO_NODEV`). | host tests | the matching key `=0` |

### Code fallback disagrees with the plan (for the engineer)

`tools/check-plan-docs.py --all` prints, for every plan key, the plan value and the fallback in code. The harness keeps the code fallback
on purpose; the Vita player build reads the plan. Fallback differs from plan for: `FX_COMPOSITE`, `PASSDOWN`, `FINCLAMP`, `TEV_CANON`, `FS_TRIM`,
`VS_TRIM`, `PIC_PREWARM`, `POOL_KEEP`, `POOL_TRIM_ONE`, `HITCH_MS`, `DYNRES_LATEHOLD`, `DYNRES_SEVERE`, `LOCK60` (off in both, but plan sets it),
`FX_LOWRES_MAXH`, `SOFTXFORM` (128 against 256), `MEGA_WARM_KIB` (16384 against 28672 in LOCKED 60), `VITA_RT_TIMERS`, `VGX_MSAA`, `LODBIAS_RUNG`, `VGX_RT`,
and the draw-count group (`CULL`, `BUILDFAST`, `FASTIDX`, `COPYINPLACE`, `CROWDTRIM`, `SKINDIRECT`, `SKINMEMO`, `UNIDUP`, `GXM_DIRECT`, `SHQUAD`, `SOFTINST`, `BLENDNODISCARD`,
`VGL_FRAGMEMO`, `VGL_UNIREUSE`, `PROMOSMALL`, `CELLCLEAR`). Always on by code without a plan row: `MASKBG`, `MASKKEEP`, `MEMFREE`, `GPU_BUSY_FIX`, `ARENA_SOFT`,
`WAIT_BANNER`, the overlap group (`np_overlap.cpp`, `NP_OVERLAP_ON`). None of this needs a code change; if a harness picture run is supposed to show the
player picture it must set these in its environment (the `pics.sh` Vita-plan env does).

## 3. OFF and waiting for the console

Each row is off by default. The run name is the kit run that judges it. "none" means no kit run exists yet and one should be added to
`matrix.py` and `night.py` before the night.

| Switch | What it would do | Kit run |
| --- | --- | --- |
| `MSC_LOCK60=1` (Settings > Picture > LOCKED 60) | The 60 policy: ladder rungs made ahead, governor, effects governor, native HUD, 60-aimed amortising. | `lock60pipe`, `lock60tour`, `lock60mega`, `lock60replay`, `lock60pix`; night `l-*` runs |
| `MSC_PICTURE_HOLD=full` | Keeps 960x544 and sheds shadows, crowd rate and effect size before changing size; 864x488 is the one visible step. | none; the judging A/B is `MSC_PICTURE=full` against `MSC_VITA_RENDER=960x544` in open play, a goal replay, a Mega Strike, Pipeline Central |
| `MSC_VGX_SBATCH=1` (+ `_ORDER`) | Static stadium props of one material in one draw; 15 to 19 draw records saved per frame in a match (host census). | `sbatch1`, `sbatch2`; night `n-sbatch1`, `n-sbatch2` |
| `MSC_VGX_ATLASJOIN=1` + `MSC_VGX_SHADOWDOWN=1` (and `MSC_VGX_ATLAS_SIDE`, default 672) | Crowd and shadow pictures share one texture, shadows at half size straight into their cell (host: 6 to 4 scenes in open play). | `scenejoin` |
| `MSC_FX_GLOW_COARSE=1` | Glow chain in 4 passes. | `glowcoarse` |
| `MSC_LOD_SIZE=1\|2` | Detail by on-screen size. | night `n-lodsize1`, `n-lodsize2`, `n-lodsize1-rep` |
| `MSC_MENU_STILL`, `MSC_PAUSE_STILL` | Hold a menu or pause picture. MENU_STILL did not engage on the tour's menus in the emulator: not a proven fix. | night `n-menustill` |
| `MSC_SHADOW_FAR=2`, `MSC_PAD_LATE`, `MSC_VGX_LEANVARY` | Far-player shadow rate; read the pad as late as possible; leaner varyings (compiled result: no clear gain, leave off). | none |
| `MSC_VGX_TEV_CANON=1` | Older simplifier mode. | matrix `canon0` for the off side |

Kit gaps found in this pass (not fixed; the coordinator has the list): the night entries `n-tevcanon2` is a no-op against the new default, `n-finclamp2`
tests value 2 where the plan is 3, `n-quadfast` and `n-gxmdirect1` repeat defaults, several plan-on switches have no OFF run in the night list, and
`night.py`'s table of "depends on branch" switches is stale. `test_kit.py` passes regardless.

## 4. Known risks, ranked

1. **Clamp removal and blended shadows.** `FINCLAMP=3` leaves out the final colour clamp where proven value ranges say it cannot matter; the proof is a
   compiler-side range analysis with a deliberate-break test (BREAK=1 and 2 are caught). If a console shows a colour band, a dark pixel or a wrong
   blend, `MSC_VGX_FINCLAMP=0` (pack 16 holds the plain forms) puts it back with no compile. The blended player shadows and shadow-copy sharing carry the
   same kind of risk: pictures differ at shadow edges in a still comparison. An unexplained difference keeps the switch off.
2. **Compositor ball fringe.** At the ball's edge in a Mega Strike the compositor leaves a one-pixel purple fringe and smoother glow streaks (judged within
   tolerance on host pictures at 6x, not on a console screen). Off with `MSC_FX_COMPOSITE=0`; the console run is `fxcomp0`.
3. **`MSC_POWERUP_HIDDEN` rests on an untested assumption**: that a power-up that is not drawn needs no per-picture work. Instruction counts show the same
   trace, but nobody has watched a power-up appear on a console with it on. First thing to look at: a power-up appearing while the camera is elsewhere.
4. **The goal-start long picture.** The first picture after a goal still builds work (render targets, glow, shadow cells). The hitch fixes (prewarm,
   keep pool) aim at it. Not measured; a long picture of about 80 to 190 ms was reported for the 1.1 notes (estimate from those notes). Look at the
   `[hitch]` lines in `log.txt` after the first goal.
5. **The online pair test has NOT passed on this tree.** Three attempts. Two failed from machine load. The third finished at 05:05 on 12 Nov (file
   `/home/claude/s13/PAIR-DONE` exists, empty; result `/tmp/omt-w8/pair.out`, `passed: false`). What it shows: both peers started, joined the fake
   service, chose Online and then Host or Join, and both stayed on the popup menu (scene 10) until the script timed out at about frame 8100 (about 10
   minutes of the game's own clock); the room scene (24) was never reached. Desync 0, sync errors 0, crash false, but also 0 matches compared and 0 sync
   ticks compared, so nothing about lockstep was proven. The machine was shared (load about 2.7 on 2 cores at the time) and frames ran at 22 to 28 ms.
   I cannot tell load from a real fault from this output. Earlier runs on an older tree passed the menu, lobby and peer connect stages, so a failure
   this early is new and unexplained: possible causes to check first are the game-start and partner-lost changes (merge `b21acb3`), the popup change from
   the text pass, or the service port setup. Action: rerun on an idle machine (command in section 5), and read `peer0/runtime.log` around the first
   `popup_menu`.
6. **Twelve matches in one boot never ran in the emulator.** The route's quit-to-menu stalls in the emulator (both 1.1 and 1.3 runs ended in match
   1), so the memory behaviour over a long session is unproven. The kit's `s-stab` (12 matches, one boot) is the first real test, on the console.
7. **Replays do not fit 60.** Replay work is the heaviest: game thread about 4 M instructions a picture after the cuts, front end 3.8 to 6.4 M in a
   celebration (instruction counts; at IPC 0.6 to 1.0 on a 444 MHz core that is about 8.6 to 24 ms, estimate, assumption stated by the harness). Expect
   replays to run at a steady lower rate. `MSC_CROWD_REPLAY=2` and the replay rate setting are the levers; a Settings row sets a steady lower replay rate.
8. **Nothing timed on a console.** Every ms in the 1.3 notes is a model. The first night may show that a default made things worse; the kit
   has an OFF run for most of them (section 3 gaps listed).

## 5. Build, package, prove a build

Commands come from `INFRA.md` and the helper scripts; all paths are on the build machine. These are heavy: wait for `PAIR-DONE`
or any other pair test to finish first, and do not run them in parallel with a pair test.

- **Python tests (light):**
  - `python3 native/platform/vita/tests/audit_settings_keys.py`
  - `python3 native/platform/vita/tests/run_settings_runtime_tests.py`
  - `python3 tools/console-1.2/test_kit.py`
  - `python3 -m pytest -q tests` (one expected failure, `test_package_matches_manifest`)
  - `python3 tools/check-plan-docs.py` (docs against plan; must print "with a doc problem: 0")
- **Vita build of a tree** (isolated logging build, title CDXMP0001, version 01.30): `SRCTREE=<tree> /home/claude/s13/tools/b13x.sh <letter>`
  (stage, GL deps, game; outputs under `/out/v13-<letter>`). `tools/build.sh` is the plain build; do not run it while others are queued.
- **Package:** `/home/claude/work/pkg.sh <letter> <name>` writes `/home/claude/work/final/<name>.vpk` using the donor package and the shader pack
  in `/home/claude/work/final/pack16` (`vgx_shaderlist.bin` and `874a13e0.pack`). **Pack 16 is not in git.** Keep a copy off the machine. It holds the plain and the
  `TEV_CANON=2` and `FINCLAMP=3` texts. For a release package: `tools/repack-release.py --app-version 01.30 --shadercache /home/claude/work/final/pack16`.
  The script prints `pkg 0 verify 0` when the stage tool accepts the package.
- **Prove a build:**
  1. Vita3K smoke: `cd /home/claude/work/v3k && TAG=<tag> FILES=/home/claude/s13/strikers/tools/console-1.2/routes/tour.txt nice -n 10 flock emu.lock ./run.sh <vpk> env/tour.txt 1500` (about 560 s). It must end with `[script] tour done` and `exit`, and the log must **not** print `GUEST FAULTS` (any count above 0 is a failure).
  2. Host pictures: `/home/claude/s13/tools/pics.sh <tree> <tag> [MSC_X=1 ...]`, then `tools/picdiff.sh <tagA> <tagB>`; two runs of the same tree must give `RESULT: IDENTICAL`.
  3. Instruction counts (null renderer on emulated ARM): `tools/armhf_build.sh <tree>`, then `tools/armhf_run.sh 4600 /tmp/x.log [MSC_X=1 ...]`. Compare step hashes (`NP_DET_TRACE`): identical, or the change touched the simulation.
  4. Online pair test (about 10 minutes, idle machine): `cd native && NP_DISC_DIR=/home/claude/work/disc/extract-strikers BIN=/home/claude/work/pics-build/strikers/host32/np-host32-full CASES=pair TIMEOUT=1500 tools/mp/run_online_menu_test.sh ...` (full line in `INFRA.md`). It passes only when `passed: true`, parts compared above 0, and no mismatch.
- Shader text must not change. Nothing in the simulation step may change. No `%zu`, `%zx`, `%lld`, `%j`. Commit as CBOC-Vita; never push without the owner's word.

## 6. Where things are

- Changes by date: `CHANGELOG.md` (draft 1.3 block). Switch table: `docs/TECHNICAL.md` (every plan key has a row; `tools/check-plan-docs.py` lists mismatches).
- Player-facing draft: `docs/RELEASE-1.3-DRAFT.md`. Owner's one page: `/home/claude/s13/w7/OWNER-SUMMARY.md`.
- Notes by topic in `/home/claude/s13/w6/` (megafx3, passes2, scenes, full, merge2, smooth, crash2, canon2, cpu60b, cpu60c, net2, kit, look, feel, fe60).
- The console kit: `tools/console-1.2` (`README.md`, `NIGHT-CHECKLIST.md`, `matrix.py`, `night.py`, `session.sh`).

## 7. The first Vita night, step by step (owner)

You need: the Vita, a charger, the computer that runs the kit, and the game data already at `ux0:data/mscharged/game`.

1. **Before you go to bed, do the checklist** in `tools/console-1.2/NIGHT-CHECKLIST.md`: charger in and battery above 50 %; Auto-Standby set to "Do not standby" and the screen-off timer to "Do not turn off"; vitacompanion running; clock profile at the stock 444 MHz; the test app (CDXMP0001) installed next to your real game (the real game is never touched); at least 2 GB free; no other app open; Wi-Fi in range.
2. **Ask the engineer to prepare** the two versions (1.1 for comparison and 1.3) and to run `python3 tools/console-1.2/test_kit.py`.
3. **Start it:** `VITA_IP=<the Vita's address> bash tools/console-1.2/session.sh night`. It checks for about a minute, then runs about 8 hours by itself. It plays the game for you with scripted routes. Do not touch the Vita.
4. **Stop it any time** with Ctrl-C; the report is still written. Run the same command again within 14 hours to continue.
5. **In the morning** read `out/MORNING.md` (one page, plain words), then `out/ISSUES.md` and open `out/looks-night.html` (the pictures).
6. **Play for ten minutes yourself** before you read the numbers: a match, a goal replay, a Mega Strike, Pipeline Central if you can. Say what you see in your words: smooth, stutter, pop-in, soft picture, flicker. Your eyes are the test.
7. **Close the lid for a minute, wake it, play one minute.** Sleep cannot be scripted.
8. Tell the engineer which of these you saw. A default that you or the numbers dislike is turned off with one line from section 2.

## 8. If the first boot fails

1. **Note exactly what you see:** a black screen, a crash code (for example C2-12828-1), a message naming a file, or a hang on the loading screen. Photograph it.
2. **A message that names a file** ("game data not found", a shader list, a disc table): the game already retried for three seconds. Check that `ux0:data/mscharged/game` is complete and `libshacccg.suprx` is installed. Try again once.
3. **Go back to the old behaviour with one file.** Open `ux0:data/mscharged/np-player.txt` and add `MSC_FPS=30` and `MSC_DYNRES=off`. If it still fails, add the off-switches for the new work from section 2, starting with `MSC_FX_COMPOSITE=0`, `MSC_VGX_PASSDOWN=0`, `MSC_VGX_FINCLAMP=0`, `MSC_VGX_TEV_CANON=0`, `MSC_POWERUP_HIDDEN=0`. Also the guard file `np-plan.txt` makes the thread plan fall back if a boot did not finish.
4. **Send the log.** `ux0:data/mscharged/log.txt` (it rotates to `log.txt.old` at 2 MiB) and any `psp2core-*.psp2dmp` crash dump. In the kit these are copied for you; do not delete them from the console.
5. **For the engineer:** read the last 100 lines of `log.txt` first; a line starting `[arena]`, `[hitch]`, `[mem]` or `[rt]` names the stage. If the log ends before the first frame, compare the package with the emulator-tested one (`sha256sum`), check `pkg 0 verify 0` was printed, and confirm pack 16 was the shader cache in the package. If a crash dump exists, symbolise it against the `.velf` of the same build.
