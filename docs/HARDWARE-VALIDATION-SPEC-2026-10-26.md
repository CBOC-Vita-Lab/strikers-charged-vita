# Strikers Three-Version Hardware Validation

## Scope And Safety

The owner authorized installation and testing on a connected Vita, with the original
game retained until the update is proven. Only one test app/package is to occupy
the device. Public 1.0, public 1.1 and the complete 1.1.1 candidate replace that
same test app in turn, following the owner's added three-way comparison request.
No public release, production-app overwrite, save migration, plugin installation,
or unrelated system change belongs to this test operation.

The USB-testing chat owns transport/plugin setup. This chat owns Strikers test
preparation and, only after an explicit handoff, Strikers device operations. Never
issue concurrent device writes, launches, or inputs. A successful USB mount or
client build does not establish safe gameplay control.

Protect `MSCN00003`, its artwork, and all of `ux0:data/mscharged` against writes.
After fresh inventory and backup, the historical test identity `CDXMP0001` and
private root `ux0:data/mscharged-perf-test` may be reused. Do not assume their
current ownership from historical notes. Shared game assets are read-only. An
owner save may be copied into private test storage after backup, never edited
in place. Never package proprietary assets, saves, or raw identifying evidence
for GitHub.

## Controlled Comparison

Stage source snapshots of tags `v1.0`, `v1.1` and the complete working fixes.
Apply the same deterministic isolation changes, compiler flags and logging
level. These are rebuilt isolated baselines, not a claim that their executables
equal published VPKs. Preserve the original public VPKs and record source/build/
package hashes and isolation diffs. Use each version's historical renderer and
dependencies, with its own shader-cache donor; do not backport 1.1 behavior into
1.0. First compare defaults as shipped, then separately matched 30-FPS policy
and settings. If 1.0 lacks a trace, use independent observation, not invented
matching telemetry. Read executable hashes; version labels alone are not identity.

Verify every writable path, including global VitaGL FFP caches and config-based
overrides. Parse the final SFO rather than trusting a filename or build flag.
Production repacking retains the old SFO and must not silently restore the
production title ID. No package with `MSCN00003` may enter this install path.

Match settings, copied save state, shader-cache state, power/clock profiles,
resolution, GPU effects, and capture mode between trials. Record battery and
temperature when available. Warm each variant, then use A-B-B-A ordering and
repeated ordinary-play samples. Start with default settings, then reproduce
legacy quality/thread overrides and the requested 20-25 FPS investigation (the
retrieved community report says 25-26 FPS). Test both The Wastelands (stadium 14)
and Sand Tomb (stadium 15) until the reporter's desert scene is identified.
Do not confuse an AUTO 60 ceiling with a promised 60 FPS.

USB capture/controller plugins may change CPU/GPU behavior. Measure capture
off/on separately; transport limits must not be attributed to the game. Do not
use virtual clocks, host cost models, lab Wi-Fi mode, or loading counters as
hardware timing evidence.

## Coverage And Evidence

Exercise ordinary friendly matches, Sand Tomb, multiple stadiums/captains,
walkouts/movies, Mega Save training and live catches, camera levels including
6/7, menus/settings/restarts, pause/resume, save/reload/cup progress, and exit.
Use copied saves and verified direct-launch scripts where available. Scripts
currently retain input ownership after EOF: disable them for manual input feel.
Missing/truncated-file fault injection is restricted to a private copied asset
tree and must never rename, truncate, or delete shared original assets.

Log each case with build/config/save/cache hashes, steps, outcome, crash/error
details, and raw evidence paths. Record explicit not-run/blocked cases. Existing
recorder banks are bounded (ordinary <=30 seconds; Mega <=60 seconds); reject
CRC errors, overflow, and raced snapshots. Long sessions require multiple valid
samples and independent observation. CPU submit/gate/game-turn rates are not
displayed FPS. Corroborate display callback/frame identities, vblank accounting,
and screen observation before making a displayed-cadence claim.

Read back protected original hashes after install and after runs. Keep raw
device backups, captures, logs, and identifying data local under ignored build
storage. Record command exit statuses and cleanup/control handoffs.

## Acceptance And Release Gate

No remaining unexplained crashes, hangs, save loss, rendering/audio regressions,
or failed fix scenarios. Demonstrate repeatable baseline-versus-fixed improvement
where a regression is reproduced, without hidden quality/clock changes. Record
ordinary-play frame-time distributions and responsiveness, not just averages.
If a user report cannot be reproduced, say so rather than claiming it fixed.

Only actual physical-Vita evidence and the owner's acceptance can open release work.
The owner identified the intended candidate as `1.1.1`. Until acceptance keep production
release metadata, public assets, tags, and GitHub releases untouched.
