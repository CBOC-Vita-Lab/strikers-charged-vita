# Strikers 1.1 Community Bug Audit

## Source And Scope

- Public repository: `CBOC-Vita-Lab/strikers-charged-vita`.
- Latest public release checked: `v1.1`, October 24, 2026, commit `5421c34ced8017d798d556ba3d8e7c6083422919`.
- Audit baseline: public main `4fbe6759ea0f255364666534551aee7fb3676549`. The intervening eleven files change documentation, helper names and checksums; runtime source and packaged VPK are unchanged.
- Original public VPK SHA-256: `1854ef4b163aace32b10b70a86a289c5e137802cf34b71569ac83a1b9595218f`.
- Local branch: `codex/strikers-1.1-community-fixes` in a fresh clone. The older publishing checkout was not updated.
- At the original source-audit boundary, no device operation or release was authorized or performed. The owner subsequently authorized one isolated physical test app, preserving the original game, with release still gated on hardware acceptance. See `HARDWARE-VALIDATION-SPEC-2026-10-26.md`; that authorization does not imply an installation or a completed hardware test.

The public performance report is one player describing 25-26 FPS on the desert stadium after updating. The cinematic report predates 1.1. Neither has a reproducible physical-Vita capture in this task. The following fixes address independently demonstrated defects, not an asserted diagnosis of either reporter's console.

## Demonstrated Defects

### 1. Hidden Legacy Picture Settings

`MSC_PICTURE=quality` or `high` expands at runtime to fixed `960x544` rendering. The old Settings model ignored the selector, displayed Standard/Default, and preserved it even after choosing Default. The explicit render size also bypasses dynamic resolution. The performance alias has the same hidden-configuration problem.

The model now uses the runtime's shared preset expansion, honors explicit keys, and replaces recognized selectors with effective settings when saved. Default/reset can then remove those settings. Unknown selectors and unrelated manual values survive. Legacy Quality does not silently acquire the newer Quality menu preset's 30-FPS cap.

A second independent review found a candidate-only boundary regression: expanding one selector could turn an otherwise valid 96-assignment file into 97 assignments, causing the runtime to ignore the trailing performance setting. Serialization now refuses output beyond the runtime's 96-assignment limit, including duplicate and empty-value assignments, rather than silently saving a different effective configuration. It does not change the runtime file format or discard owner variables.

### 2. Core Plan Recovery Ignored

Old/manual `MSC_FRAMESEND`, `MSC_VGX_BACKEND_THREAD` or `MSC_VITA_CORE_FRAMESEND` values bypass `VitaPlan::Apply`, but Settings previously displayed AUTO and retained them after choosing a named plan. `MSC_FS_PIPE=1` could also defeat C NO OVERLAP.

Such configurations now display CUSTOM. Unrelated saves retain them; an explicit core-plan selection, preset or reset clears the conflicting keys. Integration tests run the actual plan application after serialization.

### 3. Movie Request Refusal Could Latch A Permanent Wait

`readFrameAsync` set `readProgress=1` without checking whether `nlReadAsync` accepted the request. A rejected submission has no completion callback to clear that flag, so later decoder retries could wait forever while audio drains.

The flag is now cleared when submission returns null. Accepted requests are not duplicated, and callbacks completing during submission keep their completed state. The decoder, frame timing, image quality and audio scheduling are otherwise unchanged. Removing just the production guard reproduces four regression failures with the corrected test harness.

### 4. Incomplete Files Could Leave The Retail Disc-Error Wait Stuck

The SDK permanently marks failed extracted-file reads fatal. The original game's error handler waits for the drive status to change, as it could on a physical disc console. A missing/truncated host file cannot recover that way, and quiet player builds did not retain useful diagnostics.

The first read failure now retains its exact path/reason. The existing game-side fatal-status boundary displays repair instructions and exits through existing platform hooks, rather than retrying permanently. Worker/interrupt paths do not display UI. The terminal presentation must release an inherited game-thread interrupt lock; independent review identified that nested case and it has explicit regression coverage.

Normal rounded 32-byte tail reads remain successful. No per-file startup scan is added: the historical two-filesystem-calls-per-entry scan cost about 22 seconds on the maintainer's device and is deliberately not restored.

### 5. Preparation Accepted Missing Or Damaged Assets

The preparation helper checked only the executable hash, existence of `fst.bin`, and the `Art` directory. Missing files, truncated files, directories in place of files and malformed tables could still pass.

It now validates the table hierarchy/names, regular-file sizes and non-overlapping/non-wrapping disc extents before printing READY. It tolerates filename-case differences and omitted empty directories, matching the supported extraction. It does not alter game data. The supported owner's extraction passes this check read-only; its 2,351 nonempty extents have no overlap.

The second review also demonstrated that an untrusted, minimized FST could describe almost no assets and still pass this completeness check. The supported USA Rev 1 FST is now SHA-256 pinned, like `main.dol`, before its inventory is trusted. No proprietary FST bytes are distributed. Transferable `sys`/`files` roots and the two system files must not be symlinks.

The final case-sensitive Linux pass also caught a redundant literal `files/Art` precheck rejecting an otherwise valid `files/ART` directory before the case-tolerant FST walk could run. The helper now validates the `files` root and lets the authenticated FST traversal validate every directory. Missing-root errors remain explicit.

This is a completeness/structure check, not a hash of every game asset. Manual copies bypassing preparation still rely on the runtime error handling.

### 6. Frames-In-Flight Gate Could Sustain Its Own Slowdown

The input-latency gate added in public 1.1 compares the next frame's cadence against a timestamp recorded after its own wait. The late-wait pacing anchor also follows that wait. Consequently the gate can add a refresh repeatedly while its backoff sees only the usual two refreshes. The separate 30-unresolved-waits cutoff does not help when individual waits resolve successfully.

A deterministic harness includes the actual `np_fps.cpp` and models pipelined tag availability at a 30-FPS presentation stage. With the gate disabled it runs at 29.970 simulated FPS; the released gate produces 23.984 or 20.002 with two longer pipeline latencies, without triggering its cadence backoff. These are software counterexamples, not measured Vita rates or a confirmed diagnosis of the community report.

The local fix records the pre-wait timestamp instead. The existing 150-frame backoff then includes the limiter's own delay. The same two fixtures run at 29.870: occasional bounded probes still have a small cost, so this is not a promise of zero overhead. Healthy and adequately buffered fixtures remain 29.970 with no extra waits. A frozen display still reaches the existing unresolved-wait cutoff; frames before any shown tag bypass the limiter. The actual 60-FPS public flow does not arm this late-wait gate and remains unchanged. No rendering quality or frame-rate policy defaults were changed.

### 7. Startup Settings And Recovery Could Select The Wrong Runtime Plan

The final pass compiled the actual startup file loader, settings process-replacement blocks and Vita-specific plan implementation against host I/O, rather than relying only on the portable Settings model. It demonstrated these additional integration failures:

- A failed startup "Save and Start" reloaded the file without applying the selected core plan. The fallback now reapplies the plan before runtime initialization.
- A full 96-assignment file left no tracking space for derived picture/core variables. They could survive the next reread and override the newly selected plan or picture. The file limit remains 96; separate tracking storage holds those records plus all eight possible derived picture/core/timer keys.
- The generated GPU timer was not removed when a reread disabled dynamic resolution. It is now tracked without duplicating explicit owner assignments.
- Reapplying AUTO within the same launch advanced the recovery counter twice; changing to a named/manual plan retained stale automatic ownership. Reapplication now keeps this launch's original failure count and distinguishes an outstanding startup reservation from a completed success or real failure.
- An intermediate local correction abandoned an automatic reservation without restoring its prior count. Independent multi-process review caught this before packaging. Intentional named/manual transitions now cancel only the outstanding reservation; returning to AUTO re-arms it without treating the reread as another failed launch.
- Successful settings process replacement and settings-only Exit also needed cancellation before leaving the process, not just in the failed-restart fallback. Both actual process-replacement sites now cancel a pending reservation before calling the system, and restore only that still-unresolved reservation if the call returns. Deliberate startup Settings Exit cancels it too. A real failure or successful heartbeat must not be overwritten.

These are configuration/recovery correctness fixes, not a claim that a particular user's console selected the wrong plan. The regression runner persists guard files across separate executable processes and exercises successful non-returning restarts, failed startup/running restarts and intentional exits, with prior failure counts preserved.

The running restart path also races the watchdog's heartbeat/failure reporting. Three new two-thread barrier tests reproduced stale cancellation/restoration writes. Guard state and file I/O are now serialized with a directly statically initialized pthread mutex (the Vita's C++ mutex constructor is too late for EarlyBoot). Crash reporting never waits for that mutex: it publishes a deferred failure and tries the lock; a surviving holder drains the notice before changing ownership or returning, including the notification/unlock window. Independent normal, ASan/UBSan and ThreadSanitizer runs all pass the 134-case runtime suite. A faulted I/O owner that never resumes still has best-effort persistence; this is not a promise of crash-atomic filesystem writes.

## Performance Regression Leads

The public v1.0-to-v1.1 comparison contains several real changes that can affect ordinary play, not just Mega Saves:

- The default presentation filter changed from a simple vitaGL blit to SMOOTH sharpening/edge smoothing. The old blit shader contains one source texture sample; the current SMOOTH shader contains six plus edge calculations. This is not a claim of six times the GPU cost. Disabling Sharpness is a useful controlled on-device comparison.
- Dynamic resolution is new in 1.1 and can increase ordinary scenes to 1.2 times each side when its model predicts room. This adds pixel work and render-target memory relative to the old native-size default. `MSC_DYNRES_MAX=1.0` isolates this upward scaling without disabling downward recovery; `MSC_DYNRES=off` is not an equivalent test.
- New GPU-queue and frames-in-flight limits reduce input latency by restricting queued work. Queue limits can trade throughput for latency when the pipeline needs more overlap. The frames-in-flight safeguard has the separately reproduced defect described above, now corrected locally.
- The default frame-rate policy changed from locked 30 to AUTO. Rarely reaching 60 is not by itself evidence of a regression from the old default. A repeatable fall below the previous build's same-scene frame rate is the important comparison.
- Preserved `MSC_PICTURE=quality`/`high` forces 960x544, approximately 1.82 times the pixels of 640x448, and bypasses dynamic-resolution recovery. The released Settings implementation demonstrably labels this STANDARD/DEFAULT and retains the selector even after applying Default. The current local settings fix addresses this, but the reporter's configuration has not been read.
- Settings-only launches and intentional restarts could leave a false failed-start marker, so a later AUTO launch chose a more conservative thread plan. Failed process replacement could also omit the chosen plan entirely. The final configuration/recovery fixes address those demonstrated paths, but their occurrence on the reporter's console is unknown.

Clock initialization and the pinned dependency lock did not change between those tags. Without an external `hdtex.pack`, the HD loader returns before installing its lookup hook; there is no basis here to blame an absent HD pack. None of these source observations establishes a particular console's effective clocks, settings or FPS.

For hardware acceptance, preserve the current configuration first, then use the same stadium, camera, teams, mode and warm shader cache. Compare the old and new build with identical clocks and settings, then vary only one of the filter, upward-resolution cap or queue limit at a time. Do not reset every setting simultaneously and call the resulting change a diagnosis.

## Tests And Build

- Final Python suite: 56 tests; final checksum-consistent verification is recorded with the package below.
- Settings: 4,431 checks passed, including ASan/UBSan, after the second review's assignment-limit regressions. Earlier source regressions against the release baseline produce 44 failures.
- Frame-rate controller: 25 checks passed. New actual-implementation frames-in-flight fixture: 12,013 checks passed, including ASan/UBSan; removing the timestamp correction reproduces three failures. No controller policy or rendering defaults changed.
- Preparation: 23 tests passed, including missing/truncated/oversized assets, supported-FST identity, transfer-root/system-file links, malformed names/tree/ranges, bad extents, case-different files/directories, missing roots, omitted empty directories and no file mutation.
- Movie scheduler: 11 cases passed. Real `introtest.thp` and `stad_classroom.thp` player/decoder integration passed with audio/image comparisons.
- SDK ARMHF/QEMU: 24 binaries plus the input-disabled rerun passed after the nested-lock correction. The final run mounted the owner's extraction read-only and also passed real-disc reads, ten container fixtures, sound-bank mixing, three movie decode fixtures and the real movie player (25 frames, zero audio gaps). Twelve synthetic scenarios cover missing/truncated files, rounded tails, worker/virtual modes, hooks present/absent, callback deferral and inherited-lock restoration.
- Recorder fixture: fixed the standalone macOS clock-link failure; three tests passed.
- Renderer: texture, validation and program-eviction suites passed normally and under ASan/UBSan after test-only stub repairs. Display-rectangle and Z-freeze arithmetic tests also passed. The final pass repaired missing standalone player hooks in the real EGL fixture, then passed actual Mesa llvmpipe rendering at 640x528, 960x544 and 1280x1056. No GL/EGL driver function or visual assertion was replaced. No production renderer edits.
- Full Vita player cross-build passed with the existing offline Linux VitaSDK GCC 15.2 / Clang 18.1.3 toolchain and fresh pinned dependency libraries. The build reported 922 existing SDK ABI-attribute warnings (short wchar/enums), classified by the existing build recipe; these are not a hardware-compatibility verdict.
- The new public-CI movie target runs admission tests without proprietary files, then explicitly skips only real-movie integration. Both ARMHF and exact host32 invocations passed without assets and with two real movies. The final pass installed GCC 13 multilib and Mesa only in a disposable Linux container, leaving the host and original Docker image unchanged. All 24 SDK binaries plus input-disabled rerun passed in host32 and in the Vita platform's pthread-backed fake mode; both real movie-player runs logged zero gaps. The fake mode needed a missing display-header fixture; unsupported display calls now return -1 and exercise the unchanged VI timer fallback. Camera regression: 41 checks passed. CI now includes the startup integration and fake-platform targets, including the required Clang sanitizer runtime dependency.
- Second independent review identified and corrected supported-FST identity, settings assignment-limit and frame-gate cadence defects. The actual NL read API can partially admit split unaligned requests, but all 26,260 packets across the owner's 46 THP movies are 32-byte aligned; this pre-existing generic NL limitation is not presented as a supported-movie regression fixed here. Existing compiler warnings and recorder-report ResourceWarnings remain.
- Fresh sanitized settings, frames-in-flight, renderer validation and program-eviction suites passed. Cross-review of the new preparation, assignment-limit and cadence corrections found no remaining material findings. The rebuilt and repacked candidate matches the current built executable, ZIP integrity passes, and the public main/tag identities were rechecked unchanged.
- Final actual-startup integration suite: 134 subprocess/concurrency cases passed on macOS and Linux with ASan/UBSan, plus normal and independent ThreadSanitizer runs. The original successful-restart three-process reproduction now preserves the correct recovery count. All newly identified reservation and normal-thread race findings are closed; physical restart, FPS and cinematic acceptance remain separate.

Original `Strikers-Charged-Vita.vpk`, `cboc-release.json` and `docs/RELEASE.json` remain the published 1.1 artifacts. The source inventory describes the current local tree separately from that binary's provenance. CHANGELOG is not changed, so this work does not trigger the automatic release workflow.

## Unreleased Candidate

- File: `build/Strikers-1.1-Community-Fixes-UNRELEASED.vpk`.
- VPK SHA-256 after the final review: `c82bd3b496afb0491f9665c0cbf2ad3c1c86ba5bf8dfc4a714f15dba274c0b30`.
- `eboot.bin` SHA-256: `b212cbfb2d952fffeeb4d3ec2cf70c9ef6c35086ac744c2ba412a36b70e8ec38`.
- ZIP integrity passed. All eight retained non-executable/non-license members of the public release match byte-for-byte, including artwork, shader caches and `param.sfo`.
- This preserves public-package artwork, not an uninspected console's custom installed artwork. The title remains `MSCN00003`, so installation would overwrite that app. No installation was performed; obtain explicit device/test scope and preserve installed artwork and saves first.
- Original full-build log: `build/vita-build.log`. Second-review rebuild: `build/doublecheck-vita-build.log`. Fresh SDK rerun: `build/doublecheck-sdk.log`. Python rerun: `build/doublecheck-python.log`.
- Fresh real-data SDK rerun passed all 24 binaries and the input-disabled rerun. Its 25-frame movie fixture logged one initial audio gap (56 samples), unlike the earlier zero-gap run; passing this test does not establish gap-free hardware playback.
- The candidate is a successful cross-build, not a tested Vita release. No claim of improved desert-stadium FPS, fixed physical cinematic playback or device acceptance is made.
- Final build log: `build/final-audit-vita-build.log`. Final host32 SDK and Vita-platform-fake logs: `build/final-audit-host32-sdk.log`, `build/final-audit-vita-platform-fake.log`. Runtime integration logs: `build/final-audit-settings-runtime.log`, `build/final-audit-settings-runtime-linux.log`; independent sanitizer/review records are under `build/final-audit/`. Real desktop GL: `build/final-audit-egl.log`. The disposable test container was removed after verification; no host dependency installation or device operation was performed.

## Remaining Acceptance Work

1. On a physical Vita, compare the desert stadium using the reporter's exact mode, camera, settings and clock/plugin profile. Check both AUTO and locked 30 FPS. These source fixes do not prove a minimum frame rate.
2. Retest the previously freezing cinematic and normal movie/skip/loop behavior; identify which cinematic the reporter meant.
3. Verify the new fatal message is readable on-device and returns cleanly to LiveArea, with an intentionally incomplete test copy rather than the working game data.
4. Test a fresh install and a save-preserving overwrite upgrade of the exact candidate. Preserve existing artwork, settings and saves.
5. Ordinary matches, Mega Strikes/Saves, long career sessions and two-Vita play remain hardware acceptance checks, not conclusions drawn from host tests.

Online rankings and a no-Mega-Strikes option are feature requests, not implemented in this bugfix pass.
