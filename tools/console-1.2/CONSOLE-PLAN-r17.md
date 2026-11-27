# Console plan for r17: one sitting (kit20, 15 Nov; replaces CONSOLE-PLAN-r16)

All times are ESTIMATES from the kit (matrix.py route minutes, fast boot, 0.4 min a run for upload and pull, 1 min an executable swap). The first run of the sitting writes the real figures (session-times.csv). The packages are built from the final r17 by the coordinator.

## The owner's goal and what decides the order
"Close to 60 fps smooth gameplay at full resolution everywhere no matter what; the rest a smooth 30": no stutter at level load, walk-outs, goals, replays or videos, no pop-in. The 60 mode ("Smooth", MSC_LOCK60) is turned on by default only if the console proves it. So block A asks the 60 question first (and the game-thread headroom that decides it), then the owner's stutter runs. The sitting is unattended (`session.sh list`), resumable (finished runs are skipped), and each list ends by putting the owner's settings file back. Only the test title CDXMP0001 is touched: `eboot.bin` (and, only in extra unit X12, its shader pack) and the files in `ux0:data/mscharged-perf-test/`.

## What to install and prepare (on the Mac, once)
1. Nothing is installed: the lists swap only eboot.bin of CDXMP0001. The shader pack on the console is unchanged since r9. MSCN00003 and everything else are left alone.
2. Unpack the executables (names are what the lists use; a missing one skips its runs, the rest runs):
   ```
   export OUT=$PWD/out-r17                       # A NEW FOLDER FOR EACH SITTING (fable-da21 F4): finished runs and the kept owner np-player.txt live here
   python3 tools/console-1.2/prepare-versions.py --out "$OUT/versions" \
       --eboot r17=<packages>/s17-final.vpk --eboot r17pgo=<packages>/s17-final-pgo.vpk \
       --eboot r17pgox=<packages>/s17-pgo-x.vpk --eboot r17pgoxm=<packages>/s17-pgo-xm.vpk     # the last two only for extra unit X1
   python3 tools/console-1.2/prepare-versions.py --out "$OUT/versions" --vpk pack19=<packages>/s17-pack19.vpk --vpk 1.3=<packages>/s17-final.vpk   # only for extra unit X12 (swap pack19 / swap 1.3)
   ```
   (the section-ordered PGO packages are rebuilt from the FINAL tree, with a new order file name each time; the simt runs need the **simfix20 package** for r17 and r17pgo: lost wake-up fix, Fable verify10 P1; until the coordinator sends its merge the simt runs are marked "needs the simfix20 package" in every env file's header and in this plan)
3. The r17 package itself must already be installed as the isolated CDXMP0001, as for earlier sittings.

## The one command (session.sh, from a Mac with the Vita on the same Wi-Fi)
```
export VITA_IP=<the Vita's address>
export OUT=$PWD/out-r17        # a folder that does not exist yet; never reuse the folder of an earlier sitting (its old logs would count as finished and `restore` would put back an old settings file)
bash tools/console-1.2/session.sh check
bash tools/console-1.2/session.sh list tools/console-1.2/r17-sitting.txt          # blocks A and B, ends at the natural stop
bash tools/console-1.2/session.sh list tools/console-1.2/r17-sitting-extra.txt    # only if there is time: units X1... in order of value
```
Nothing to click on the Vita. If it stops (Wi-Fi, sleep, battery), run the same command again. Fast boot starts every stadium run straight in the match. The Vita's own save is never written during these runs. The default OUT is `./out-r17`, one folder per sitting. After the last list, check that np-player.txt on the console is the file kept at THIS sitting's first run (`restore` does it by hash against `$OUT/state/owner-np-player.txt`). Every list ends with `restore` (the test app's own settings file is put back as it was). Every block A run and the 60-mode runs also write `MSC_HITCH_LINES=1500` (log volume only). Every run is checkable from its own log: the env file names the switch under test (`[vita] np-player.txt: KEY=VALUE`), and a run whose switch line is missing is INVALID, never PASS.

## Where it stops
| Stop after | Time (est.) | What you have |
|---|---|---|
| Block A | about 2 h 17 min (setup 2, 137 min of runs and swaps) | the 60 answer for Vice, Pipeline Central, The Dump and Lava Pit with the headroom lines; the weld and the heat-shimmer cuts against their OFF sides; the in-match settings screens; whether the match step on its own thread boots at Vice; the stutter fixes at the stadium load and goal cuts, the held scenes, cut learning |
| Block B = the NATURAL STOP | about 3 h 05 min (A 137 + B 48; the cap is 3 h 15) | plus the PGO answer (also at 60 and with the overlapped step). The plain r17 executable is back and the owner's settings are restored |
| Extra X1-X3 | + 81 min (cum. incl. setup) | section-ordered PGO, the settings screens that ship, the rest of the simt runs |
| Extra X4-X8 | to 273 min | the Dump's GPU hunt, the GPU levers, video memory, probes, heat shimmer |
| everything in the extra list | + about 10 h | see the table below |
The Smooth decision needs only blocks A and B.

## Block A (137 min, 31 runs including the 3-run goal slot) in order
1. `eswap r17`; references `d1`, `worstgpu1`, then **`dlweld0`** (the vertex weld is a plan default now and sits ON in every run of the sitting; this is its OFF side: fable-da21 F7).
2. **The 60 answer:** `lock60tour`, `lock60pipe`, `lock60dump`, `lock60lava`, each with MSC_HEAD_STAT=1 (the `[head]` line: game-thread p50 / p95 / max per scene class with and without a step, render-thread p95, GPU p95, pictures over 16.7 ms by cause). Right after `lock60lava`: **`warboff-l`** (the heat-shimmer cuts are ON by default and never ran on a console: its OFF side; fable-da21 F6). Then `lock60gs-v` (Vice tour) and `lock60gs-d` (The Dump): LOCK60 + HEAD_STAT + GSPLIT=2 + FX_STAT.
3. **The screens every 1.3 player gets:** `live19-leave` (the leave dialog on a next-start row) and `scene18-pause` (the pause menu's VITA SETTINGS screen), both driven by the route.
4. **The match step on its own thread** (needs the simfix20 package): `simt-blk-v` first (MSC_SIMT=1: boots, plays, `[simt]` counters), then `simt-v` (MSC_SIMT=2 MSC_RENDER_RNG=2). Every simt run carries MSC_OVERLAP_LOG=1 (without it the `[fs] np_sim` line the decision rule reads never prints, and `[simt] final` relies on atexit, which the console exit does not run: a simt run without it is INVALID). `simt-d` (The Dump) is the first run of X3.
5. **The owner's stutter fixes (r16's block A):** `crep1`; the discard pair (two a side, alternating); `ram112-dump`, `nodepth1` (The Dump first: partial-render risk), `ram112-a`, `rehearse6`, `loadhold1`, `scenepin1` twice; `heldaa13w2` twice, `cutlearn2` twice; the goal-scenario slot (`gsvice16`, `gsdump16`, `gslava16`: a goal, celebration and replay in the first minute).

## Block B (48 min): PGO against the plain build, alternating
`pgo-base-d1-a`, `pgo-base-wg-a`, `pgo-base-lock60`, eswap r17pgo, `pgo-on-wg-a`, `pgo-on-d1-a`, `pgo-on-lock60`, `simt-pgo`, eswap r17, `pgo-base-wg-b`, `pgo-base-d1-b`, eswap r17pgo, `pgo-on-d1-b`, `pgo-on-wg-b`, eswap r17, restore. WATCH THE FIRST 60 s OF THE FIRST PGO RUN FOR A CRASH. The plain runs are also repeats of the block A references (their noise). `pgo-*-lock60` use the lock60gs-v env: only the executable differs.

## What moved to the extra list, and why
- Moved out of block A to keep it near 2 h 15 while adding 11 runs (4 lock60 moved in from block C, 2 gs, 2 simt, `dlweld0`, `warboff-l`, `live19-leave`, `scene18-pause`): the third a side of the discard pair (stall10 is ON by default and measures a gain; four runs give the direction), `smooth0` (a switch-off diagnostic of the smooth clock) and `heldaa13` (the r15 side of a question the owner already decided: one sample in held scenes). They are in X10 and X9. `simt-d` moved to X3: the overlapped step cannot enter the Smooth table from this sitting, so the settings screens every player gets took its place.
- Moved out of block B: the third a side of the PGO pairs (two a side, plus the new 60-mode and simt pair, plus the d1 / lock60 noise from block A) and the whole section-ordered PGO unit (`pgox-*`, about 26 min with its two swaps): to X1, the first unit, because the decision "ship the order-only build" needs the block B figures first.
- Block C of r16 no longer sits in the main list: the sitting's natural stop is the end of B (it was 2 h 45 for r16, it is 2 h 57 now for much more: the 60 answer, simt and the 60-mode PGO). All of block C and D is now X1-X15 in the extra list, in order of value for the owner's goals; pacing (r16 block D, expected about no gain) is last (X15).
- `lock60mega`, `lock60replay`, `lock60pix`, `gpubusy0`, `between30` stay in matrix.py as env files (not in a list): the tour run already contains a Mega Strike and replays per class.

## Extra list (r17-sitting-extra.txt), minutes are estimates, cumulative includes 2 min setup
| Unit | Runs | Min | Cum. | What it answers |
|---|---|---|---|---|
| ON | 6 | 15 | 15 | the REAL online service (online24), one console, no opponent: `on-hub`, `on-hub-old` (MSC_ONLINE_HOST=old), `on-lists`, `on-quick`, `on-ranked`, `on-report` (SAVE A REPORT); `on-conn` is by hand |
| X1 | 6 | 26 | 26 | section-ordered PGO (`pgox-d1-a/b`, `pgox-wg-a/b`, `pgox-lock60`) against block B's pgo-on runs; `pgoxm-d1` (SDK maths in the LTO unit): ship order-only unless xm gains more |
| X2 | 3 | 15 | 44 | the settings screens that ship, route-driven: `live19-shake`, `live19-crowd`, `live19-rear`; slot front19 (the leave dialog and the pause screen are in block A) |
| X3 | 8 | 37 | 81 | more simt (only if simt-blk-v was clean): `simt-d` first, then `simt-lava`, `simt30-d`, `simt-lat`, `simt-c1`, `simt-c12`, `simt-fs12`, `simt-c3`; slots pace18, simfix20 |
| X4 | 22 | 125 | 206 | the Dump's GPU hunt dumpgpu20 S2-S14 (`dg20-*`), `swz-d` / `swz-v` / `swz-m` (swizzled textures, off in the plan), `rainoff-d` after S6, the gpuscene20 diagnostics `scenes-d`, `scenes-v` LAST (the emulator showed 47 guest faults with all four on: a fault there is a diagnostic failure, not a game failure); slots swz20, shadowst19. rainhull19: no runs (the hull stays off) |
| X5 | 5 | 20 | 226 | exact switches with verify (`exactall-vice/-dump`: every verify count 0), then `lock60x-dump`, `lock60x-lava` (LOCK60 + HSR_PROVEN/FACTS + RENDER_NEON; the weld is a default now) |
| X6 | 7 | 36 | 262 | video memory: `nodepth1v`, `rtpool1`, `megartpool1`, `memsite-d`, `memsite-v`, `patcherbuf` (2 MiB patcher buffer), `pbl1` (parameter buffer in LPDDR); slot alias19 |
| X7 | 5 | 16 | 279 | price17 / probe18: `price17ref`, `price17a` and `probe18b` (the AI clock read as before: the OFF side of the default), `price17b`, `probe18a` (all with GSPLIT=2) |
| X8 | 4 | 8 | 286 | heat shimmer (the Lava off side `warboff-l` is in block A): `warb-l` (cuts, LITE 0), `warboff-s`, `warb-s`, `warbl-s` (Sand Tomb, fb-sand: LITE 3); lock60lava of block A is the "plan default on" side at Lava |
| X9 | 16 | 56 | 345 | r16 candidates not in A: `heldaa13w1`, `heldaa13`, `scenepol1/s`, `heldscn3/7`, `cad3/d`, `bpverify`, `lat16a-d`, `popin1`, `l17d`, `c17base` |
| X10 | 10 | 35 | 379 | overclock A/B `clock444/d`; game-thread off sides `bpfull0`, `aiavoid0`, `snap0`, `rneon1`, `aiavoid7`, `rneonv`, `bpfull1`, `smooth0` |
| X11 | 11 | 43 | 423 | `freezeidle`; census19 `fxc0-*`, `fxtight-c0-*` (compositor off both sides); The Dump start step `dp16a/b` three a side |
| X12 | 1 | 6 | 429 | `swap pack19`, `l17pack19` (pack19 + MSC_WARM_CAP=500), `swap 1.3` |
| X13 | 8 | 43 | 472 | cut learning pairs, `megad1`, `megasharp0/2`, `crowdgtoff`; slot dumpgpu10 |
| X14 | 17 | 65 | 537 | r15 cuts (each twice), dump-all, render streams, bg30 runs, memory runs, `lat16e`, `l17a-c` |
| X15 | 24 | 101 | 638 | pacing, three rounds at Vice and The Dump (expect about no gain) |
Each unit that swaps an executable ends with the plain one back. Slots (below) reserve minutes in their unit only as a comment line until their runs exist.

### Unit ON: the real online service (online24)

One console, no opponent. Needs the executable built from agent/online24 (MSC_NET_REQLOG; without its `[np-net] req` lines `online_check.py` says INVALID). Every run sets MSC_ONLINE_HOST=auto (`on-hub-old`: old), MSC_NET_REQLOG=1 and a preset MSC_ONLINE_NAME (nothing is typed). The first sign-in of the sitting makes the test account on the live service; the second on-hub run proves the stored token.

| Run | What it answers | Min |
|---|---|---|
| on-hub | (a) online menu opens, profile signed in, new address answers | 1.2 |
| on-hub-old | (a) the same on the old address only | 1.2 |
| on-lists | (e) friends list and rankings, read-only | 2.0 |
| on-quick | (c) PLAY WITH ANYONE: join, 60 s, Circle cancels, menu back | 2.7 |
| on-ranked | (d) ranked queue, the same | 3.0 |
| on-report | (f) Settings, MULTIPLAYER page, SAVE A REPORT (console only) | 1.0 |
| on-conn (by hand) | (b) connection check: its only entry is the failure popup after a failed HOST A ROOM, so use a Wi-Fi with no internet | 1.5 |

Pass rules (`python3 tools/console-1.2/online_check.py <log> --run <id>`; exit 0 PASS, 1 FAIL, 2 INVALID): the `[np-net]` lines show requests answered by the service (HTTP 2xx or its own JSON errors); no "trying the other address" line in an auto run (a fallback line means the new address failed: report it); no crash; the menu returns after cancel; at most 100 requests per 10 s (the zone rule) and well under the day's budget; any pollScale / level / retryAfterSec is listed. Host proof (local fake service, `online_host_proof.py`) covers the routes; the real answers are console only.

## Decision rules
- **Smooth (the 60 mode) ships for a stadium class** when its lock60 run has p50 16.7 ms, late under 2 %, no relief in open play, `[head]` game-thread p95 under 14.5 ms, and `[split] held == finished, leaked 0` (split17). A class that relieves stays Detailed-only in the menu. `MSC_LOCK60` goes on by default only if the console proves it. Check that the `[head]` render-lane samples are not 0.0 and the with-step column has samples (both read empty in the emulator). Relief flapping (a 60 try every 32 s) read from lock60dump / lock60lava: if tries fail repeatedly, the controller change "30 for the rest of the match after 3 failed tries" is next.
- **simt (overlapped step)**: run `simt-blk-v` first; any fault or a counter not 0 stops all simt runs. A single `JOIN TIMEOUT` line right after a PS-button suspend is noise, any other JOIN TIMEOUT is a fail. MORE late pictures in a simt run means a suspected heap-lock sleep, not "no gain". Turn it on for a stadium class only when its simt run has game-thread p95 at least 2 ms under the matching reference, late % not higher, no audio underruns, `[simt]` counters clean; for the held 30 when `simt30-d` lowers late pictures. **MSC_SIMT never goes into the Smooth table from this sitting** (Mega Strike race P2 stays open unless simfix20 closes it). All simt runs need the simfix20 package.
- **PGO** goes in when it is faster on both routes and at 60 and does not crash. PGO plain-against-PGO det traces must be compared in the emulator on fb-dump before the PGO twin is offered (-fprofile-use turns on -funswitch-loops). The section-ordered build: ship order-only unless `pgoxm-d1` gains more; run `vita_hot_order.py check` on the final link.
- **Handoff rule** for every other switch: ON when its runs show fewer `[cuts] first use` lines and shorter `[hitch]` gaps at the walk-out or goal cuts, no more late pictures in play, no new `FAILED` / `TRIMMED`, no crash. A visible change (warble LITE, bg30, scenepol, heldscn bit 4) needs the owner's eye.
- Do not judge a run that printed `pack MISS` (a shader missing from the pack is compiled once and cached): run it again.
- A memory switch (ram112, nodepth1, rtpool1, cut learning) wins only if the `FAILED` counts do not rise. nodepth1 is judged first: any fault, crash or corrupted picture and it is never used, and `nodepth1v` is skipped.
- Verify runs (`bpverify`, `aiavoid7`, `rneonv`, `exactall-*`) must show `wrong=0` / mismatches 0; any other number keeps the switch off.
- **price17 / probe18 (MSC_AI_NOCLOCK=1 is the plan default since r17)**: `price17a` and `probe18b` are the OFF side of the default (the AI script clock read as before, 80 reads a picture); `price17ref` is the on side (with GSPLIT=2). NOCLOCK stays on when `price17ref` is not slower than `price17a` (the `[gsplit]` begin-frame + sim ms), with no new `[hitch]` and equal `[fps]` decisions; if `price17a` is clearly faster than `price17ref`, the default is wrong and goes off. `probe18a` (probe every 4th picture, clock default) and `probe18b` (probe every 4th, clock OFF side) against `price17a`: the probe's own gain is `probe18a` against `price17ref`; its stale readings may move a decision by up to 4 pictures, so the `[fps]` decisions must stay equal.
- **Swizzled textures (swz20; MSC_VGX_TEXSWIZZLE stays OFF)**: `swz-d` / `swz-v` against `worstgpu1` / `d1`: `[lane] gpu` lower at The Dump, every HUD icon right, `[texclass]` counts present (INVALID if missing). Any wrong icon = stop the swz runs (`swz-m`, mip chains, runs only if `swz-d` was clean). MSC_VGX_TEXSWIZZLE=1 goes into the plan only after a clean console round shows lower GPU ms at The Dump and no wrong icon. (Env files: MSC_VITA_SHOT values with `/` are silently dropped by the env loader; the kit uses comma lists.)
- **Vertex weld (MSC_VGX_DLWELD=1 is the plan default since r17; fable-da21 F7)**: `worstgpu1` is the on side and `dlweld0` (block A, right after it) the OFF side. Pass rule: the `[dlweld]` line present in worstgpu1 (INVALID if missing: the weld did not run), no new `[hitch]` at the walk-out and goal cuts against `dlweld0`, no fault or broken stadium or character geometry in any run of the sitting. The weld stays on unless the on side is slower or faults; a geometry fault anywhere is read against `dlweld0` first (every run of blocks A and B carries the weld).
- **Heat shimmer** (warble19, ON in the plan: the three exact cuts and LITE 1; fable-da21 F6): `warboff-l` (block A, after `lock60lava`) is the OFF side. The `[acts]` line (every 3600 pictures, the first at picture 600) must show `EFBSWAP_NEXT` swaps and `CLEARVEH` clear-only copies above 0 on the on side (lock60lava; if its first line is still in the walk-out read gslava16 at picture 4200): a 0 means the cut did nothing on the console. No `[efbswap] ... little video memory` and no new FAILED lines in gslava16. Keep ON when `[lane] gpu` and late % are not worse than `warboff-*`; the 30 Hz map (LITE 1 at Lava = `lock60lava`; LITE 3 at Sand Tomb = `warbl-s`) is the owner-approved cheaper look: if the shots look wrong, report. (`[acts]` needs no key: it is always printed, so MSC_VGX_PASSMEMO_LOG is not added.)
- **dumpgpu20 (Dump GPU)**: S5 near the base means fixed work (go to the per-scene line and S9); S4 minus S5 large and S4 far under base = shading + texture cost (then S7 / S8 say filter or count); S6 gain of 2 ms or more = blended draws (`rainoff-d` says how much of it is rain: it needs `[hunt] rain` above 0), shadowst19 first; `scenes-d` / `scenes-v` (per-scene GPU ms, `[pb est]`, texture layout) run last: the emulator showed 47 guest faults with all four diagnostics on, so a fault there is a diagnostic failure, not a game failure; if the Dump's S2 / S3 intercept is within 2 ms of Vice's, stop looking for fixed costs. `flat`, `pixel`, `blend`, `upskip` must print that they acted (not DID NOTHING).
- **census19**: tight polygons are only invisible with the compositor off (`fxtight-c0-*` against `fxc0-*`); with the compositor on they change the picture.
- **freeze button**: `freezeidle` must show no FATAL line; `freezebutton` (by hand) must show FATAL + thread lines and a `psp2core-*.psp2dmp` in ux0:data.
- **live19**: the log shows one `[live] KEY=VALUE -> cell N` line per applied row, the route's `STATE` lines to the end, no second boot line (no restart), no `[hitch]` right after a save. The routes' row positions come from fb-live19.txt's pitch (49 px) and were not seen on a console: a missing STATE line makes the run INVALID.
- **l17pack19**: only a boot-cost and first-use trade; ship pack19 only if present-2 time does not grow by more than the first uses it removes at the goal / replay / walk-out.
- Left out on purpose: nodepth2, rainhull0 (the rain hull is OFF and unproven: MSC_RAIN_HULL crashes when on).

## By hand (needs a person, 15-25 min with the Vita in hand; not in any list)
| item | what to do | what it answers |
|---|---|---|
| `freezebutton` | `bash session.sh run freezebutton`; hold L+R+Select for 4 s, 30+ s into the Dump walk-out | the FATAL + thread lines and a psp2dmp in ux0:data (kubridge loaded) |
| scene18 by finger | in a match press Start, VITA SETTINGS with the pad and then with a finger; change a row; page 2; DONE with a change (popup) | the touch reaches the new screen; the text is readable on the OLED; (`scene18-pause` is the route-driven twin) |
| scene18-restart | change SMOOTH 60, DONE, SAVE AND RESTART (outside a match only: in a match live19 has no restart) | the restart works on hardware from the popup |
| scene18-all | ALL SETTINGS from page 2, leave the technical screen, come back | it opens over the paused match and returns to the panel |
| menufix17-smooth | pause, VITA SETTINGS, PICTURE page: SMOOTH 60 MODE ON, read FRAME RATE / REPLAYS rows, SAVE AND RESTART, play 5 min | the restart and the row text |
| menufix17-pause / -livearea | pause with a finger; Circle (and Square with CLASSIC) leave; the LiveArea Settings button while the game runs and from a cold start; hold L + R at start | the three entries still work; the dialog is readable |
| live19-rear by hand | with `live19-rear` running, touch the back panel before and after the toggles | the back panel stops and starts |
| sc19 (startcheck19) | its note has no console lines, and the guard is OFF in every kit run (MSC_BOOT_MATCH / MSC_INPUT_SCRIPT switch it off): by hand, with a changed Picture line in the test app's own settings file, start the game and close it within 2 s (PS button) twice | the third start says `2 FAILED STARTS: STANDARD PICTURE AND LANGUAGE` on the loading screen and reads the standard lines; a normal start writes nothing. Unproven on a console: owner's call |
| `dispdepth` (vram19) | `bash session.sh run dispdepth` (MSC_VGX_DISPDEPTH=nomem); play a match, open the keyboard (online name entry) and the ad hoc NetCheck dialog | the dialogs are drawn; the `display depth:` made / freed counters (about 15 min) |
| mpdiag18-a / -c | SETTINGS > MULTIPLAYER > SAVE A REPORT (online mode: `Report saved: ux0:data/mscharged/mp-report.txt`, no IP / name / code in the file); Connection Test needs a local fake and is not offered here | the report; the verdict sentences |
Restore the owner's np-player.txt (`bash session.sh restore`) after any by-hand `run`.

## Two-console list (only if the owner has a second Vita; NOT part of the sitting)
`tools/console-1.2/r17-lan.txt` lists the legs (`bash session.sh lan <leg>`, `export VITA_IP2=<second Vita>`, about 4 min a leg). Radio runs (Nearby host and join as in the LAN plan; base env is the LAN pair, `lan/` files):
| run | env | what it answers | minutes |
|---|---|---|---|
| `lan-base`, `lan-auto` | none / MSC_LAN_INPUT_DELAY=auto on the host | the references; mpdiag18-b = `lan-auto` then, after the match, SETTINGS > MULTIPLAYER > SAVE A REPORT on both: events show session start, draft, input delay, match-end digests | 4 + 15 |
| `lan-simt-both` | MSC_SIMT=2 MSC_RENDER_RNG=2 MSC_SIMT_LAN=1 MSC_OVERLAP_LOG=1 on both | the overlapped step in a session: same score, no desync message, `[mp-lan]` sync ticks 0 mismatches (needs the simfix20 package; without MSC_SIMT_LAN=1 the switch is refused: one `[simt] refused` line, the shipping behaviour) | 4 |
| `lan-simt-host` | the same on the host only | consoles with it on and off may play each other | 4 |
| radiofix15 | MSC_LAN_RADIO_FIX=15 | does Nearby start, stay up and close cleanly; the first sender MAC in the peer list, no odd source port, no PdpRecv error | 10 per pair |
| radiofix0 | MSC_LAN_RADIO_FIX=0 | control (the 1.2 radio) | 10 per pair |
| lantimeout | MSC_LAN_GROUP=0 MSC_LAN_TIMEOUT_MS=15000 | the NetCheck dialog without a group name, shorter time-out | 3 |
| keepawake | (default MSC_MP_KEEP_AWAKE=1; base env: the LAN pair or online) | sit untouched 6 min in the ranked or quick match queue, or a Nearby / Same Wi-Fi lobby, with the system's dimming and auto-standby at their shortest: screen stays on, no suspend, session holds; the log shows one `[mp] keep awake on`, then after leaving `[mp] keep awake off (N ticks)` and single player dims as before. Control: MSC_MP_KEEP_AWAKE=0 dims and suspends as today | about 8 |
| mpfix18 checks | keep sceNet after Nearby (MSC_NET_KEEP_STACK default 1): after a Nearby session the Online menu / leaderboard / Connection Test must work without a restart; NetCheck cancel and keyboard cancel must not soft-lock; log the longest stall of a Dump / Lava load on a stock console | by hand |
| delay19 | SLOT: input delay; coming later: kit20 adds the legs to `lan_text()` and this table | |
Rule: turn the radio bits on only if radiofix15 connects at least as often as radiofix0 and logs no new error; bit 1 can be judged alone by a deliberate cell loss (switch the other console off mid-session, start Nearby again within 30 s: the radio must reach DOWN and a new session must start).

## Slots still open (helpers running; the coordinator sends the run lines, kit20 / the integrator adds them to matrix.py, sitting.py `OPEN_SLOTS` and this table; each shows as a marked comment line in its unit)
| slot | helper | unit | reserve (min) | what it will answer | lines |
|---|---|---|---|---|---|
| rainhull19 | rain | - | 0 | rain hull stays OFF; no runs | closed |
| shadowst19 | stadium shadows | X4 | 10 | the shadow quad at The Dump / Lava | to fill |
| vram19 | video memory at goal cuts | X6 | 5 | `patcherbuf`, `pbl1` (a hang = fail), `dispdepth` (by hand) | filled |
| alias19 | depth sharing | X6 | 10 | | to fill |
| gpuscene20 | Dump GPU diagnostics | X4 | 10 | `scenes-d`, `scenes-v` (last in X4), `rainoff-d` (after S6); the emulator showed 47 guest faults with all four diagnostics on, so a fault there is a diagnostic failure, not a game failure | filled |
| swz20 | swizzled textures | X4 | 15 | `swz-d`, `swz-v`, `swz-m` (the swizzle stays OFF; any wrong HUD icon = stop the swz runs; MSC_VGX_TEXSWIZZLE=1 only after a clean console round shows lower GPU ms at The Dump) | filled |
| front19 | OPTIONS button | X2 | 5 | | to fill |
| pace18 | sim thread core map | X3 | 10 | MSC_VITA_CORE_SIM against simt-v (needs simfix20) | to fill |
| simfix20 | simt step fixes | X3 | | the package the simt runs need | to fill |
| awake19 | keep awake | two-console list | 8 | line above (`keepawake`), already filled | filled |
| delay19 | input delay | two-console list | | | to fill |
The route16 goal-scenario slot is live (block A). The dumpgpu10 stadium-cut slot (X13) joins by itself when its ids exist.

## Caveats
- Fast boot shows the tour without the menu scenes and with a pinned random seed, so d1 is not comparable with older d1 logs for the menu stretch; the walk-out, play and goal are.
- A run that crashes does not stop the sitting; it is logged and the next starts (three dead consoles in a row stop it). session.sh has no gate: after a faulting `simt-blk-v` the later simt runs still run and are voided by the rule above.
- Not seen on a console: the route-driven settings routes (live19c / live19r / live19l rows), `fb-sand.txt` (fb-lava with stadium 15, written by hand), and every simt run on the stock-console cores.
- Dry runs (nothing sent anywhere): `bash tools/console-1.2/vm/dryrun/dryrun.sh` (the Mac-VM queue of the main list: 41 runs, ALL PASS) and `DRY_RUN=1 bash tools/console-1.2/session.sh list r17-sitting.txt` / `r17-sitting-extra.txt` (41 / 145 launches).

## Driving it from the Mac VM (qkit15, updated for r17)
`q.sh` advances a queue one slice (170 s per command) when a Claude session drives the Vita; files are in `tools/console-1.2/vm/` (queue `queue-r17.txt`, env folder `kit-r17q`, made by `python3 tools/console-1.2/vm/mkqueue_r17.py` from `r17-sitting.txt`; the extra list is not queued: run it with session.sh). Setup is as in r16 with `r17` / `r17pgo` for the executables and `$V/kit-r17q`, `$V/queue-r17.txt`; the loop `cd $HOME/x/w9 && export VITA_IP=... KITDIR=$HOME/x/w9/kit-r17q && bash q.sh 150` until QUEUE EMPTY; `STOP POINT` is the natural stop (the plain executable back, the owner's settings restored); `HALT` and `DEFER` as in r16. Main list: 41 runs. Each sitting gets its OWN state folders so an old owner copy is never reused: `q.sh` works in `$HOME/x/w9/outq-r17` (a fresh `out/owner/np-player.txt` is saved from the console at the start of each sitting, never carried over from an earlier one) and the relay keeps its state in `relay-r17` (STRIKERS_SITTING=r17; STRIKERS_RELAY_ROOT overrides). A new sitting uses a new name for both.
