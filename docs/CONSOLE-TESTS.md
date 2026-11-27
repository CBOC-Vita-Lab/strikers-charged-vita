# Console Tests For The Next Optimisations

The remaining slow scenes are limited by the graphics chip. The emulator cannot measure that, so
the next changes depend on numbers from a real Vita. Every switch below is a line in
`ux0:data/mscharged/np-player.txt`. Remove it afterwards.

The 1.2 checklist (routes, switches, pass rules) is at the end of this page: [Version 1.2 console checklist](#version-12-console-checklist). The 1.3 runs are in [Version 1.3 console checklist](#version-13-console-checklist).

Measure with a logging build (`NP_LOGGING=1`: `[fps]` lines in `log.txt`), or with the performance
recorder (`MSC_TOUCH_DIAG=1`, see [Diagnostics](DIAGNOSTICS.md)). For each switch, play the same
scene twice: once without the line and once with it.

| Question | Scene | Switch | What it tells us |
| --- | --- | --- | --- |
| What does the sharpening cost? | A match and the main menu | default, then `MSC_VITA_UPSCALE=off` | Expect well under 1 fps difference. If it costs more, make OFF the default again |
| Does Mega Saves hold 30 fps? | Strikers 101 > Mega Saves | default, then `MSC_MEGA_RENDER=off` | The default should hold 30. With `off` it shows the old speed. If the default looks too soft, try `MSC_MEGA_RENDER=512x352` |
| Does a match reach 60? | A full match | default (AUTO), then `MSC_FPS=30` | Logging builds print `[fps] auto: 30 -> 60` and `[vgx] dynamic resolution: ... level` lines. If 60 is rare and the reason is a CPU core, the GPU levels cannot help |
| What do the GPU timers cost? | A match | `MSC_DYNRES=off` | The timers add one notification per render scene. Expect no visible difference |
| Is the larger picture worth it? | Main menu, a match at 30 | `MSC_DYNRES_MAX=1.0` | The default may draw up to 768x544 when the GPU has room. If that ever costs frames, cap it |
| What do the low-resolution effects save? | Strikers 101 > Mega Saves | default, then `MSC_FX_LOWRES=0` | The default should be much faster. `MSC_FX_LOWRES=4` saves more if it is still slow |
| Does the anti-aliasing pay off? | A match, the main menu | Settings > Anti-Aliasing 2X | Smoother edges, small but visible. Keep it only if the frame rate holds |
| Does anisotropic filtering work? | A match | `MSC_VITA_ANISO=0` by hand (the Settings row went in the 1.3 prune) | The far half of the pitch should look crisper. If textures look wrong, the filter values are not what we think |
| How much does the picture size cost? | Mega Saves, main menu, stadium select | `MSC_VITA_RENDER=480x336` | A big gain means fill rate (pixels). Little change means geometry or render-pass switches |
| Is the touch and stick delay gone in Mega Saves? | Strikers 101 > Mega Saves | default, then `MSC_VITA_QUEUE=off` | The logging build's `[lat]` lines give pad-to-screen delay; `[vgx-vita] GPU queue depth` lines show when the limit drops to 1 |
| Can input feel quicker still? | Mega Saves, a match | `MSC_VITA_DISPLAY_BUFFERS=2` | One frame fewer waits in the display queue. Check that the frame rate holds |
| Is the 30 fps lock even? (1.2) | A full match at `MSC_FPS=30`, a scene that was 20-25 fps before | default, then `MSC_LAT_ANCHOR=0` and `MSC_LAT_DEPTH=0` | The `[vgx-vita] display:` lines say how long each picture was shown (2 vblanks = even 30). `MSC_LAT_ANCHOR=0` is the 1.1 position of the cadence anchor |
| Does AUTO still flip between 30 and 60? (1.2) | A full match, a stress stadium | default, then `MSC_FPS_HYST=1` | Count the `[fps] auto:` lines per minute, and the 30/60 share of the `[vgx-vita] display:` windows. Default: down after 4 frames over 92 %, up after 2 s within 80 % |
| Is UP TO 60 better than the held 30? (1.3) | The same scripted match, scenes of 38-50 fps | default, then `MSC_FPS_BETWEEN=pace` | `[fps] between:` lines, `[present]` line (judder, shown-minus-start, `L`), `[vgx-vita] display:` holds, `[lat]` p50. Keep it if pictures rise by a quarter or more, judder stays under 0.1 and `[lat]` rises by under a refresh. Then `MSC_PRESENT_SYNC=1` shows what sync alone gives |
| Does the 500 MHz profile stay? (1.3) | Start with and without a PSVshellPlus profile | default, then `MSC_VITA_CLOCK_POLICY=fixed` | The `clocks:` line in `log.txt`: with the profile "requested ARM none" and about 498 measured after; without it 444 requested and measured |
| Are replays and celebrations even? (1.2) | Goal replays, celebrations, the intro NIS | default (`fit`), `MSC_FPS_REPLAY=30`, `MSC_FPS_REPLAY=auto` | The `held for 0/1/2/3/4+ vblanks` counts during the replay: the share of 2 should be near 100 % at 30 |
| Do the larger pictures cost frames? (1.2) | Main menu, a match at 30 | default, `MSC_DYNRES_RULES=1`, `MSC_DYNRES_MAX=1.0` | `[vgx] dynamic resolution: ... level` lines: the larger levels (0-2) should be rare and short; no step-down within seconds of a step-up |
| Does a different buffering change the delay? (1.2) | Mega Saves, a match | `MSC_VITA_TRIPLE=0`, `MSC_VITA_DISPLAY_SCENES=2` | `MSC_VITA_TRIPLE=1` is the default (3 buffers); `0` is the same as `MSC_VITA_DISPLAY_BUFFERS=2` |
| Is the menu limited by geometry? | Main menu | `MSC_VITA_RENDER=480x336` | The main menu draws about 105,000 triangles per frame (a 3D scene of about 52 draws), then an 8-pass glow chain |

What a main-menu frame contains (traced in the emulator with `MSC_VGX_PASSES=<frame>`):

- **3D background:** 52 draws, about 104,700 triangles, at 640x448. This includes ten copies
  of a 4,600-triangle mesh and two large meshes of 8,800 and 14,000 vertices.
- **Glow chain:** a down-sample to 320, 160, 80 and 40 pixels, then up-sample back. Each step
  is its own render pass.
- **Composite and interface:** about 35 to 50 draws of about 1,000 triangles.

If the resolution test barely helps there, the triangle count is the cost. Lower-detail menu
models, or drawing the menu background less often, would then be the next things to try.

## Version 1.3 Console Checklist

The console kit (`tools/console-1.2`, [its README](../tools/console-1.2/README.md)) runs all of this unattended, about 3 h 40 min, and can stop after `dmid`. The pass rules are those of 1.2
below. Nothing here has been measured on a Vita. Reference: the kit's run in the same route (`d1`, `dmid`, `d2`; `pipe1`, `pipe2`; `rep1`, `rep2`; `megad1`, `megad2`).

| Kit run | Line | Today's default | Judge it on |
| --- | --- | --- | --- |
| `softx128`, `softx64` | `MSC_VGX_SOFTXFORM=128`, `=64` | 256 | `[lane]` front-end ms and p50 against `d1`. Galaxy's biggest single step (128 to 32), not a Strikers figure. Upload hitches in the first seconds reject a value. Do not try `0` |
| `gxmdirect1` | `MSC_VGX_GXM_DIRECT=1` (Settings > Advanced > GXM Direct) | off | Median, p95, back end ms; `[gxm-direct]` refused count 0; no corrupt geometry |
| `between` | `MSC_FPS_BETWEEN=pace` (Settings > Frame Rate > UP TO 60) | off | The UP TO 60 row of the table above |
| `snaponce0`, `posecull0`, `crowdposeonce0` | `MSC_FS_SNAPONCE=0`, `MSC_POSE_CULL=0`, `MSC_CROWD_POSEONCE=0` | on | Game thread ms. On stays on unless a run with the line is faster than the noise |
| `crowdrefresh1` | `MSC_CROWD_REFRESH=1` | 2 | The one visible cut of the group: if this run is no slower than `d1`, 2 goes back to 1 |
| `npclazy0` against `perfhide` | `MSC_NPC_LAZY=0` with `MSC_PERFORMANCE_MODE=1` | on | Only acts with the crowd hidden, so the reference is `perfhide`, not `d1` |
| `promosolo8`, `dltiny64` | `MSC_VGX_PROMOSOLO=8`, `MSC_VGX_DL_TINY=64` | off, 15 | Front end and back end ms; hitches at the start of a stadium |
| `fxglowhalf` | `MSC_FX_GLOW=half` (Settings > Advanced > Glow) | full | `menu-main` p50; first see in `[memo]` whether the pass memo already skips the chain there. Glow by eye |
| `shadow2`, `farlod12` | `MSC_SHADOW_EVERY=2`, `MSC_REPLAY_FARLOD=12` (Advanced > Replay Shadows, Replay Far Players) | off | `rep:replay` and `rep:nis` against `rep1`, `rep2`. By eye: a stale shadow after a camera cut, a mesh that pops |
| `crowdreplay2`, `clusterlazy` (tier 2) | `MSC_CROWD_REPLAY=2`, `MSC_CROWD_CLUSTER_LAZY=1` | off | `rep:replay` and `rep:nis` against `rep1`, `rep2`. `clusterlazy` must look identical. By eye for `crowdreplay2`: the crowd's stutter close to the camera |
| `menulod1`, `menulod2`, `menulod3` (tier 2) | `MSC_MENU_LOD=1`, `2`, `3` | off | `menu-*` p50. `3` against `d1` says how much of the main menu is the backdrop world (the emulator trace: 52 draws, about 104,700 triangles). `1`, `2` act only where captains are shown. By eye: every captain's textures present, flatter faces, half-rate captains |
| `hazecache3`, `hazecache3heavy` (tier 2) | `MSC_FX_HAZE_CACHE=3`; with `MSC_FX_HAZE=1` (the heavy-stadium profile is the build default on Pipeline Central) | off | `pipe-play` against `pipe1`, `pipe2`, and against `defaults-all-pipe`. First check `[amort] haze, 300 frames:` drawn about 100, held about 200. By eye while the camera follows the ball: mist that swims or steps, mist now in front of what it went behind |
| `shadowplay3` (tier 2) | `MSC_SHADOW_PLAY=3` | 2 | `[rt] scenes` per frame and `play` p50 against `d1`, `d2`. By eye: the legs of a running shadow |
| `amort60`, `amort60ref` (tier 2) | `MSC_AMORT_60=1` with `MSC_FPS=60`; `MSC_FPS=60` | off | `[rt] scenes` per frame, `play` p50, frames at 16.7 ms. By eye: crowd and shadows as they look at 30 |
| not in the kit yet | `MSC_MENU_STILL=1`, `MSC_PAUSE_STILL=1`, `MSC_STILL_LOG=1` | off | `menu-*` p50 against `d1`; `[still] ... held`; `failed` stays 0. By eye: captains in front of the world, a change of screen with no stale frame, pause and resume |
| `pipeheavy0` | `MSC_STADIUM_HEAVY=none` | `8` (build default, with `MSC_STADIUM_HEAVY_CROWD=0`) | `pipe-play` against `pipe1`, `pipe2`: the price of turning Pipeline Central's profile off. The log must not say `[heavy] stadium 8: heavy profile on`. The disc shows the outlier (`docs/PIPELINE-CENTRAL-AUDIT.md`: Mist_rain, modelled at 5.9 screens); `pipecensus` confirms it: `[fgcensus] play/efb/fx` near 1.7 Mpx. By eye: the mist fuller, the crowd as in the other stadiums |
| `pipemist0` | `MSC_FX_TIGHT=0 MSC_FX_TIGHT_DROP=0 MSC_FX_GOV=0` | off today; the proposal (branch `defaults-proposed`) turns them on | `pipe-play` against `defaults-all-pipe` (the proposed set on) and `pipe1`: p50, GPU ms, `[vgx] dynamic resolution` steps and mean scale. Pipeline Central is the players' top complaint (6-7 fps on 1.0/1.1); if the proposed set is still under 30, `hazecache3heavy` next |
| `floorbake0` (tier 2) | `MSC_FLOOR_BAKE=none` | `7,8` (build default: Lava Pit, Pipeline Central) | `pipe-play` against `pipe1`, `pipe2`. Check that `[floorbake] stadium 8` is absent. By eye: the full moving reflection of the pitch. Slower here means the default pays |
| `floorsheen0` (tier 2) | `MSC_FLOOR_SHEEN=none` | `vertex` (build default with the floor bake) | `pipe-play` against `pipe1`. `[floorbake] ... sheen: 0 per vertex`. GPU time should match `pipe1`; the game thread or front end pays the per-vertex work. By eye: the pitch flat while the camera moves |
| `hazewide` (tier 2) | `MSC_STADIUM_HEAVY=3,14 MSC_STADIUM_HEAVY_CROWD=0 MSC_FX_HAZE_WIDE=1` | `MSC_STADIUM_HEAVY=3,14 MSC_STADIUM_HEAVY_CROWD=0` | Crystal Canyon and Wastelands. By eye: caustics or ash grainy |
| `fxruns2` | `MSC_FX_LOWRES_RUNS=2` | no limit | `mega-save` p95; `[fxstat]` runs per buffer |
| `clockfixed` | `MSC_VITA_CLOCK_POLICY=fixed` | `measure` | At stock clocks equal to `d1`. Then, by hand, with a PSVshellPlus profile: the `clocks:` line says requested ARM none and about 498 measured (`ALLOW_CLOCKS=1`) |
| `pipecensus`, `ctrlcensus`, `pipe1`, `pipe2` | The census lines of the kit's `env/pipecensus.txt`; `pipe*` plain | | R3. The census's frame times are void. `pipe-play` fps is the first real figure for this stadium in 1.3. `[vgx-vita] GPU queue depth ... the GPU takes X ms`: X near the frame time is GPU, far below it is CPU or uploads |
| `min070`, `fx4`, `perf12`, `perf13`, `pipeperf` (tier 2) | PERFORMANCE (60)'s `MSC_DYNRES_MIN60=0.70`, `MSC_FX_LOWRES=4` with `MSC_FX_LOWRES_MINPX=325000`; `perf13` and `pipeperf` also carry `MSC_FPS_BETWEEN=pace`, `MSC_VITA_EDGE_AA=0`, `MSC_SHADOW_EVERY=2`, `MSC_REPLAY_FARLOD=12` | in the mode | Median and p95, and the sizes in the `[vgx] dynamic resolution:` lines: a floor never reached is no win. Softer ball and players, chunky glow or halos at depth edges reject it. Run before 1.3 ships: these two values are in the mode unmeasured |

Not timings:

| Check | How | Pass |
| --- | --- | --- |
| Settings rows | Each choice of the 29 rows of the 1.3 prune (five pages) | The `[run]` header shows the line. The rows that went are tested by the kit's env files, not by the screen |
| A saved PERFORMANCE | A 1.2 file saved under PERFORMANCE (60) | The guard turns off MSC_FPS_REPLAY, MSC_FX_LOWRES*, MSC_VITA_ANISO once (`np-player.before-1.3.txt` keeps the original); the rest still runs |
| Eyes with the HD pack | With `hdtex.pack`: a walk-out close-up, default, then `MSC_HDTEX_FACES=1` | Blinks look right; `hdtex: N eye frames left as the game has them` in the log, N about three per character. With `=1`, look for the eye flicking to a softer picture at a blink |

## Version 1.2 Console Checklist

Version 1.2 added many switches. Host tests prove that they do not change a picture or a result; only a Vita can say whether
they are faster. This section is the list of what to run, how to run it, and when a result counts. The rows tagged (1.2) in the
table above belong to it too.

### How to measure

Every comparison follows the same rules, so two sessions can be compared.

- **Same Vita, same state.** Same clock profile and battery state, same Settings file and save, same stadium, teams, captains and camera. No capture or
  controller plugin unless the plugin is what is being measured. Use a logging build (`NP_LOGGING=1`, `[fps]`, `[lat]` and `[vgx-vita]` lines in `log.txt`)
  or the recorder (`MSC_TOUCH_DIAG=1`, [Diagnostics](DIAGNOSTICS.md)). The log's `[run]` header names the build and the settings in force; keep it with the numbers.
- **Warm first.** Play the route once before measuring so the shader cache is filled (and, after an update from 1.1, moved). A switch that changes shader keys
  (`MSC_VGX_ATRIVIAL`, `MSC_VGX_COPYSHADERS`, `MSC_VGX_SKINPAL`, `MSC_VGX_Q1`) compiles its new shaders on the first run: that run is the warm-up, not a result.
- **Order A-B-B-A.** A is the reference (the line absent), B is the switch. Run A, B, B, A, and keep the two A runs: they measure the noise.
- **Steady windows.** A result is a window of **10 seconds of steady play**: no scene change, pause, cut or camera change, no 30/60 change and no picture-size change
  (unless that is what is being measured). Take at least three windows per run.
- **No shader compile.** A window in which the log shows a shader compile (`[vgx-vita] shader work so far:` with a higher compile count, or the progress bar) is thrown
  away and taken again. The first goal replay of a cold cache is a test of the cache, not of a switch.
- **Three numbers per window:** the **median**, the **p95** and the **p99** of the presented-frame interval in milliseconds (the display windows in the log
  and the recorder's display cadence; `tools/diag/perf-report.py` prints the median, p95 and maximum, and p99 is taken from the same intervals: a 10 s window at 30 fps
  has 300, so p99 is the third slowest). For latency questions add the `[lat]` pad-to-screen p50 and p95; for pixel questions the GPU time per frame.
- **Noise is twice the spread of two reference runs.** For each statistic, noise = 2 × the absolute difference between the two A runs. A difference between A
  and B that is smaller than that is "no change", whatever its sign.
- **Deciding a default (the rule these tests use).** Turn a default on only when (1) the statistic the switch is meant to improve is better than the noise on its
  route, (2) no statistic on any route in the regression set below is worse than the noise, and (3) for a switch that changes the picture, the picture was compared
  side by side and accepted. A switch whose gain is inside the noise keeps its current default. A default that is already on is turned off when it fails (2).
- **Looking is part of the test.** For every switch that touches drawing, compare the same frame by eye (or from a capture) between A and B, and note any missing
  geometry, flicker, banding or softness. "Pictures equal" on the host is a different, weaker statement.

### Routes

| Route | What it is | Used for |
| --- | --- | --- |
| R1 Open play | Friendly match, a stadium with a crowd, 60 s of open play after kick-off (the scripted navigation in `native/tools/fps/nav/` can drive it) | Most CPU and queue switches |
| R2 Desert | R1 in Sand Tomb, then The Wastelands (the reported 25-26 fps scene) | Heat distortion, the 30/60 policy |
| R3 Pipeline Central | R1 in Pipeline Central (the reported 6-7 fps scene) | The worst-case scene for the GPU |
| R4 Stress | R1 in the heaviest stadium and the heaviest camera (zoom 7) | Dynamic resolution, queue depth, culling |
| R5 Lock 30 | R1 with `MSC_FPS=30`, a scene that was 20-25 fps before | The 30 fps lock fix |
| R6 Replays | A goal, its replay and the celebration, for each captain, at zoom 7 | Replay class, skinning, shaders |
| R7 Mega | Strikers 101 > Mega Saves, and a Mega Strike in a match (shot and save) | Effects pass, picture size changes |
| R8 Cut-scenes | Intro movie, the walk-out of two stadiums, a stadium preview | Movies, depth of field, effects |
| R9 Menus | Main menu, team select, stadium select | Pass memo, geometry |
| R10 Two Vitas | A full local match, Nearby and Same Wi-Fi | Local play |
| R11 Stadium change | Match in stadium A, back to the menu, match in stadium B | Cache release, memory |

### Switches whose default must be decided

These are still off. Reference (A) is always the line absent. "Targets" is the statistic that has to improve (rule 1); the regression set is R1, R4, R6 and R7.

| Switch to try (B) | Today's default | Routes | Targets / also look at |
| --- | --- | --- | --- |
| `MSC_AUDIO_PRIO=112` | 128 | R1, R6, R8 | p99; audio gaps and crackle (the main risk) |
| `MSC_VITA_TRIPLE=0` (kit run `dbuf2`, tier 2), `MSC_VITA_DISPLAY_SCENES=2` (kit run `dscn2`, tier 1) | 3 buffers, 1 scene | R7, R1 | `[lat]` p50 and p95 against frame interval p95 |
| `MSC_VGX_GXM_DIRECT=1` | off | R1, R4, R6, R7 | Median and p95, back end ms; no corrupt geometry; `[gxm-direct]` shows a refused count of 0 |
| `MSC_VGX_ONESTREAM=1` | off | R4, R6 | Median and p95; no corrupt geometry (needs a vitaGL built with `DRAW_SPEEDHACK`, the default build) |
| `MSC_VGX_CULL=0` | on | R1, R4 in three stadiums | Back end ms, p95; no missing geometry at the screen edges, in replays and during camera cuts |
| `MSC_VGX_VGL_FRAGMEMO=0`, `MSC_VGX_VGL_UNIREUSE=0` | on | R1, R4 | Back end ms; pictures unchanged (no wrong blend, no stale colours) |
| `MSC_DYNRES_SEVERE=0` | on in the player build | R4 in the heaviest stadium | Time spent under 20 fps; `severe overload` lines; picture softness while it holds |
| `MSC_VGX_SKINPAL=1` | off | R6, R8 | Median and p95 in celebrations; shaders compile once on the first run |
| `MSC_VGX_Q1=1` | off | R1 | Only if the gain is above noise; a few pixels differ by one level |
| `MSC_VGX_GXD=0x3FFFFFFF` (adds merge-by-serial and the resolution fast path) | `0x3FFF5FFF` | R1, R4 | Median; they did not pay on the armhf instruction counts, so expect no change |
| `MSC_DYNRES_FG_STEP` / `MSC_FG_LODBIAS=4` (the LOD step is not offered by default) | off | R4, R3 | p95 against the default ladder; the `[vgx] background step` lines. Try `MSC_FG_LODBIAS` 2, 4, 8: the unit is believed to be 1/8 mip and has not been measured |
| `MSC_FX_LOWRES_SCENES=nis` with `MSC_FX_LOWRES_MINPX=480000` (no Settings row since the 1.3 prune) | off | R8, R6 | Cut-scene p95; glow compared by eye |
| `MSC_FX_WARBLE=half` | full | R2 (Sand Tomb, Lava Pit walk-out and celebration) | p95 during the distortion; softness of the whole picture while it runs |
| `MSC_FPS_REPLAY=30`, `=auto` (Settings > Replays & Cut-scenes) | `fit` | R6 | Share of 2-vblank pictures during the replay; any dip below 30 |
| `MSC_LAN_INPUT_DELAY=auto`, then `=2`, `=6` (since delay19 each is the start value and floor: the `[mp-lan] net` `delay=avg [min-max]` must read about 2 / 6 and differ between the runs; both consoles run LAN `MSCLAN/4`) | unset (starts 4, settles at 2) | R10 | `delay=` avg/min/max, starved steps and stalls in the `[mp-lan]` windows; how the input feels |
| `MSC_LAN_DRAIN=1`, then `MSC_LAN_RETRY_US=3000` (delay19, both default off) | off | R10 | `starved=` in the `[mp-lan] net` line against the run without; `drain=on/runs/datagrams`, `retry=<us>/tries/ok/us`; `refused=` (ad hoc sends the radio answered WOULD_BLOCK) |
| Put the Vita to sleep (PS button, then the power button) and wake it, in a match and in a menu; also open LiveArea with the PS button and return | `MSC_AUDIO_RESUME` on | R1 | Music and effects come back within a second or two; `[audio] output port reopened` in the log; with `MSC_AUDIO_RESUME=0` note whether the sound stays away (that is what the switch fixes) |
| `MSC_VITA_STALL_NOTICE=5` (test value: message after 5 s, the longer one after 15 s) | 20 | any scene with a long first-use shader compile | A long compile (10 s and more) shows the message on the frozen picture; it vanishes when pictures return, never covers a fatal-error report, and the match goes on; `[vita] stall notice level` in the log |
| On a PS TV: boot, title, menus, a match, a Mega Strike and a Mega Save with the pad only | `MSC_PSTV` unset | R1, R3 | `[plan] PS TV` line in the log; no stray rear-tap actions on a stick click; the Triangle control guide lists buttons only |
| `MSC_VITA_CORE_GC=1` (collector and mixer on core 1) | unset (core 2) | R1, R4 | Median, p99; the `[lane]` player and back-end columns; the `audio` and `Garbage Collector` lines of `[threads]`. `NP_VITA_CORE_AUDIO=2` with it moves the collector alone |
| `MSC_VITA_CORE3=1`, with and without CapUnlocker | off | R1, R4 | `[vita] fourth core:` line; without the plug-in it must say `not available` and `[threads]` must match a run without the key. With it, median, p99, audio crackle (then `NP_VITA_CORE_AUDIO=2` if crackle or stalls appear: the mixer holds the interrupt lock for part of its work) |
| T8 core 3 rules: `MSC_VITA_CORE3=1` (rules on) against `MSC_VITA_CORE3=1 MSC_VITA_CORE3_RULES=0` (the 1.3 placement), then `MSC_VITA_CORE3=1 MSC_VITA_CORE3_DISC=1`, then `MSC_VITA_CORE3=1 MSC_JOBS=3 MSC_JOBS_CORES=23` (a job lane on core 3) | off | R1, R4, the online hub | `[vita] fourth core rules` and `[vita] core 3 rules:` lines; 8 launches per setting for freezes; median and p99; the hub and a queue with the key on: `[vita] core 3 rules: an online or LAN session is up: N thread(s) moved off` then login and pings normal |
| `MSC_VITA_CORE3=1 MSC_VITA_CORE3_SEND=1`, with CapUnlocker; then `MSC_VITA_CORE3_SEND=1` alone; then `MSC_VITA_CORE3=1 MSC_VITA_CORE3_SEND=2` (ON + FRAME SEND: also `[vgx-vita] thread "vitaGL Garbage Collector" ... moved to core mask 0x80000: ok`) | off | R1, R4 | `[vita] fourth core: usable` and `thread "frame send" on core 3`; median, p99, `[lane]` columns against `MSC_VITA_CORE3=1` alone and against neither; a late-frame or send-join (`[fs]`) increase means the system's tasks on core 3 hurt it: then `MSC_VITA_PRIO_FRAMESEND` lower. Without the plug-in the line must say `not available` and `[threads]` must match a run without the key |
| `MSC_DVD_DIRECT=0` against the default, `MSC_DVD_CHUNK_KIB=128` and `=0`, `MSC_DVD_PREOPEN=1` (needs `MSC_DVD_TRACE=1` for the read list) | direct on, 512, no pre-open | a match load from the menu, then a stadium change | Seconds from "Play" to the first match frame (`MSC_LOAD_TRACE=1` timeline), the `[dvd] loading:` line, `[nlwait]` lines with a play-time wait. The boot-to-title time too. Same picture and the same load order: a different order means a bug |
| Put the Vita to sleep during a match load, wake it | `MSC_DVD_RETRY` on | R1 | `[dvd] read of ... failed ... reopening and trying again` and `recovered after N attempts`, followed by a finished load, where 1.2 ended in the disc error (`MSC_DVD_RETRY=0`) |
| `MSC_PREFETCH=15` (kit run prefetch15), then `1`, `2`, `4`, `8` alone | 0 | R1 | Game thread ms of `[lane]` and p50 against d1; the route must end the same; keep the bits that lower the game thread |
| `MSC_FS_TLSFAST=0`, `MSC_PERSP_TAN=0` (kit run armcpu0); `MSC_VITA_FPSCR=1` (kit run fpscr1); `MSC_FS_TLSVERIFY=1` once | 1, 1; 0 | R1 | Game thread ms of `[lane]` and p50 against d1 (the first pair is exact, the route must end the same); the `[fpscr]` line of any run says what the main thread starts with; with TLSVERIFY no `MISMATCH` line |
| `MSC_ODE_BROADPHASE=0`, `MSC_POSE_FAST=0` (kit run gamefast0) | 1, 1 | R1 | Game thread ms of `[lane]` and p50 against d1; the simulation is the same, so the route must end the same |
| `MSC_FX_TIGHT=8`, `MSC_FX_TIGHT_DROP=1` (kit run fxtight) | 0, 0 | Pipeline route | GPU ms and p50 of `[frame-timing]` against pipe1, pipe2; `[fxtight]`: share saved; the picture must not change: `tools/console-1.2/looks.py` or the same shots from the emulator |
| `MSC_FX_GOV=1` (kit run fxgov) | 0 | Pipeline route | `[vgx] fill governor` lines, fewer `[vgx] dynamic resolution` steps down than pipe1, late frames; look at the mist: grain or flicker is the cost |
| `MSC_AUDIO_NEON=0` | 1 | R1 | Cpu ms of the `audio` thread in `[threads]` at the same frame count; the reverb must sound the same |

### Defaults that are already on (confirm, or turn off)

Run B as the line set to the value that restores the 1.1 behaviour, and apply the same pass rules. A row passes when turning it off is not better than the noise.
The first group was switched on for 1.2 as the best expected experience, before any console measurement (the decision is to change one back if testing finds a
major problem): these are the rows that most need the A/B. Put the line in `env.txt` (the console kit in `tools/console-1.2` has a file for each).

| Switch to try (B) | Routes | Targets / also look at |
| --- | --- | --- |
| `MSC_DYNRES_FG=0` (no Settings row since the 1.3 prune) | R4, R3 | p95 against the default; the `[vgx] background step` lines; players and HUD must stay sharp |
| `MSC_VGX_MSAA=0` (Settings > Anti-aliasing OFF), `=2` (2x only) | R9, R5 | Frame interval p95 against the default; edges by eye; how often the ladder reaches the multisampled rungs. Use the measured GPU time of 2x and 4x against 1x to correct `MSC_VGX_MSAA_COST` (model: 1.3,1.9) |
| `MSC_DOF=1` (the game's capture), `=0` (no blur) (Settings > Depth Of Field) | R8 | Cut-scene median and p95; the default (`lazy`) must look identical to `1`; `MSC_DOF_LOG=1` lists where it is used |
| `MSC_VITA_ANISO=0` (no Settings row since the 1.3 prune) | R1, R3 | Median; ground and wall textures by eye (the filter values are believed, not proven, to be anisotropic) |
| `MSC_VGX_SHQUAD=0` | R1, R4 | Median and p95 in a stadium with many shadow volumes; pictures should be identical |
| `MSC_VGX_SOFTXFORM=128` (the 1.1 limit) | R1, R4 | Median, renderer-thread time per frame; geometry by eye (not `0`, which is no CPU transform) |
| `MSC_MEGA_CURVE=0` (and `MSC_MEGA_TRAP=0`) | R5 Mega Save at 30 | Catches from a standing start, overshoot when the stick is released, the hand's reach to the corners and the diagonals (wave 6 feel); the default must feel finer at a small push and equal at full |
| `MSC_PAD_LATE=1` (`padlate`) | R1, R5 held 30 and LOCKED 60, single player | `[lat]` p50 and p95 fall by about the slack (model 12 ms at 30, 11 at 60); `[fps] late latch:` line (delay, late pictures); the picture rate and `[present]` judder unchanged; stop at once if the rate drops |
| `MSC_LAT_DEPTH_PACED=0` | R1 in the UP TO 60 scenes of 38-50 fps | `[lat]` p50 against throughput (`[fps] between:` pictures a second); keep it only if pictures fall by under a tenth |
| `MSC_CAM_INTERP=0` | R1 open play with a dribbler | Camera shake against the players by eye while a player runs with the ball at 60 and 30 |
| `MSC_SLIP=0` | R1, R5 | p95 and p99 interval; how a late frame feels. Single player only |
| `MSC_VITA_QUEUE_FIXED30=0` | R5, R4 at `MSC_FPS=30` | p95 and p99 interval; `[lat]` p50; the `GPU queue depth` lines (depth 2 held at 30) |
| `MSC_VITA_QUEUE_WAIT=event` | R4, R5 | `[vgx-vita] GPU queue:` lines (waits, ms each on average, timeouts 0, no "polling" line); p95/p99 interval and `[lat]` p50 equal or better than without; render-core busy in `[lane]` lower |
| `MSC_PACE_VBLANK=0` | R5, R6 | Share of pictures shown for exactly 2 vblanks; p99 |
| `MSC_VITA_PRIO_VBLANK=0` (the timer role's priority) | R1, R4 | p99 interval; listen for audio crackle |
| `MSC_VITA_CORE0_ALONE=0` (first `log` to list the threads) | R1, R4 | Median and p99 |
| `MSC_VGX_PASSMEMO=0` | R9, R1, R7 | Menu and open-play median. `MSC_VGX_PASSMEMO_LOG=1` says why a pass was drawn again; the skip counts are in the desktop `MSC_VGX_GLSTATS` census |
| `MSC_VGX_CLEARDEFER=0` | R9, R1 | Median |
| `MSC_VGX_CELLCLEAR=0` (kit run `cellclear0`) | R1, R4 | Open-play median and p95, GPU time; the crowd-impostor cells' clears are one quad draw per run with the switch on (shader cache seeded with it on, or the glClear stays); look for a stripe or a dark/odd rectangle in the crowd |
| `MSC_VGX_RSCACHE=0`, `MSC_VGX_TEXMEMO=0`, `MSC_VGX_REGSHADOW=0` | R1, R4 | Renderer-thread time per frame; median |
| `MSC_VGX_ATRIVIAL=0`, `MSC_VGX_COPYSHADERS=0` | R1, R9 | GPU time; these change shader keys, so warm first |
| `MSC_VGX_GXD=0` (the 1.1 front-end paths) | R1, R4 | Median and p95. One coarse pair for all front-end bits; split it only if it shows a difference |
| `MSC_THP_HWJPEG=0` | R8 | Movie frame interval, audio continuity; the per-movie `ready / late / kept / decoded on the game thread` counts |
| `MSC_THP_NOWAIT=0` | R8 (stadium preview) | Game-thread stalls while the preview plays |
| `MSC_VGX_DLFLUSH_STADIUM=0` | R11 | Free memory at the start of the second match; any hitch in its first seconds |
| `MSC_FPS_HYST=1`, `MSC_DYNRES_RULES=1`, `MSC_LAT_ANCHOR=0` | R5, R1, R4 | The (1.2) rows in the table above |
| `MSC_VGX_ALLOC_GUARD`, `MSC_VITA_ROBUST`, `MSC_VITA_RT_RING_GUARD`, `MSC_VGX_DLCACHE_BOUNDS_GUARD` | none | These change only what happens after a failure. No pairing is needed: if any pop-up, missing draw or crash appears, repeat the route with the switch at `0` and report both logs |

### Build options (separate packages)

Each one needs its own package, built from a clean build root, and the same A-B-B-A rules against the default package. `VGL_EXTRA_OPTS` changes the vitaGL id string and with it
the shader cache key, so that package needs its own shader pack ([Building](BUILDING.md)).

| Build option | Routes | Targets / also look at |
| --- | --- | --- |
| `VGX_OPT=-O3` | R1, R4 | Median and p95; package size |
| `VGX_LTO=thin` | R1, R4 | Median and p95; start-up time |
| `NP_LTO_OLEVEL=3` | R1, R4 | Median and p95 |
| `VGL_EXTRA_OPTS=-DINDICES_SPEEDHACK` | R4, R6 | Median and p95; no corrupt geometry |
| `NP_CLANG_PGO=<profile>` | R1, R6 | Median and p95; the profile covers no Mega Strike and no replay, so R6 and R7 must not be worse |

### Checks that are not timings

| Check | How | Pass |
| --- | --- | --- |
| Update over 1.1 | Install the 1.2 package over a console that has run 1.1 | Saves and settings are kept; the log lists the moved shader files once; the next launch finds `cache-moved`; no progress bar at the first goal replay |
| Settings rows | Each new row and each choice, an old 1.2 PERFORMANCE file, Save and Restart, a copied 1.1 settings file | The `[run]` header (`gfx:`, `settings:`, `controls:`) shows what each choice wrote; a 1.1 preset's fixed size shows on Resolution; Default clears it |
| Language | Settings > Language > Spanish and French, restart | Menus and text change; no missing characters; sound and movies unchanged |
| No Mega Strikes | Mega Strikes OFF in a friendly | A charged shot is an ordinary shot; Strikers 101 keeps its Mega Strikes |
| Movies | Every intro and stadium preview, with skip | Picture and audio continuous; the first-frame check reports no mismatch |
| Missing file | A copied game tree with one file removed, then one cut short (never the real tree) | A message naming the file, then the LiveArea; the real tree is untouched |
| Local play, two 1.2 Vitas | R10 in Nearby and in Same Wi-Fi, with Mega Strikes OFF on both | A full match without DESYNC; same score on both |
| Local play, refusal | A 1.1 Vita with a 1.2 Vita; two 1.2 Vitas with different Mega Strikes settings | Neither lists the other; the log names the kind of mismatch |
| Sleep in a match | Sleep one Vita for 10 s mid-match, wake it | The other Vita learns of it at once; both reach the lobby cleanly |
| The 30 lock | R5 against 1.1 on the same scene | Pictures shown for 2 vblanks at about 100 %; no slow stretch |
| HTTPS trust (needs a build with `NP_MBEDTLS`) | `MSC_NET_SELFTEST=https://<any public HTTPS host>` for hosts with a Google Trust Services, Let's Encrypt and an SSL.com certificate | The `[net-https]` lines show the handshake, cipher and certificate; a wrong date on the Vita gives the clock message. This is the only console test of the TLS stack that needs no service |

