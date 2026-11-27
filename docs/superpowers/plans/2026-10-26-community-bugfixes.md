# Strikers 1.1 Community Bugfix Plan

> **For agentic workers:** Use systematic debugging and test-driven development. Keep fixes local and obtain an independent final code review.

**Goal:** Find reproducible defects related to the community's performance, cinematic and incomplete-install reports, fix demonstrated causes, and document hardware-only questions.

**Architecture:** Start from public main `4fbe6759ea0f255364666534551aee7fb3676549`. Its runtime source and VPK match release `v1.1` (`5421c34ced8017d798d556ba3d8e7c6083422919`); intervening changes are documentation and helper names. Preserve existing platform hooks, settings precedence, asynchronous I/O and gameplay rules.

**Tech Stack:** C++ Vita platform/SDK, Python preparation and verification tools, Linux VitaSDK GCC 15.2 / Clang 18, host and ARMHF/QEMU regression tests.

**Spec:** User request in this task: inspect the latest Git version and find/fix the previously researched errors. A report is a lead, not proof of its cause.

## Constraints

- No GitHub push, release replacement, Vita installation or device settings changes.
- Preserve original contributor work, public VPK, user assets, saves and settings.
- No speculative reduction of visuals, simulation changes or blanket effects suppression.
- Tests and builds do not establish physical-Vita FPS, latency or gameplay acceptance.
- Do not add online rankings or optional gameplay modes during bug triage.

## Tasks

### 1. Release And Baseline

- [x] Verify public latest release, main SHA and runtime difference.
- [x] Create a fresh isolated checkout and local bugfix branch; do not update the older publishing checkout.
- [x] Run the existing Python/portable C++ tests and record failures.
- [x] Repair the demonstrated recorder-test clock-link failure without changing production timing.

### 2. Settings Migration

**Files:** `native/platform/vita/src/np_settings.cpp`, `native/platform/vita/tests/test_settings.cpp`; existing `np_picture_presets.h` is the runtime authority.

- [x] Reproduce a recognized legacy `MSC_PICTURE=quality` remaining active after Default/reset despite the menu showing Standard.
- [x] Write and run failing tests for effective display, explicit-key precedence, unrelated-save preservation and reset/default behavior.
- [x] Expand recognized legacy presets into the settings model using the same runtime mapping, then serialize effective keys without retaining a hidden recognized selector.
- [x] Preserve unknown selectors/manual keys and keep legacy Quality's original frame-rate behavior.
- [x] Run the settings suite and renderer/frame-rate regression tests.
- [x] Reproduce and fix hidden low-level overrides defeating explicit core-plan selection; preserve them on unrelated saves.

### 3. Movie Read Admission

**Files:** `native/ref/RVL_SDK/thp/THPSimple.cpp`, `tests/movie/thp_skip.cpp` and its existing harness.

- [x] Demonstrate rejected async read submission leaving `readProgress` latched without a callback.
- [x] Add a failing regression for rejection/retry and ensure a genuinely in-flight read is not submitted twice.
- [x] Clear the in-progress flag only when no read was accepted; retain seek, audio, queue and deadline behavior.
- [x] Run movie scheduler, player/decoder and SDK tests.
- [x] Add the data-independent movie read-admission regression to CI.

### 4. Incomplete Game Data

**Files:** `PREPARE-GAME.py`, `tests/test_prepare_game.py`, plus a narrow runtime error path only if its cause is demonstrated.

- [x] Trace loading errors: startup checks only the existence of `fst.bin` and `files`; failed DVD reads permanently set fatal drive state, while the original game waits for that state to change.
- [x] Add synthetic extracted-disc fixtures showing missing/truncated assets passing the existing preparation check.
- [x] Validate the extracted file-system table and its listed regular-file sizes before declaring preparation ready, without modifying owner data.
- [x] Preserve fast normal startup: do not restore the 2,637-file, approximately 22-second per-launch scan.
- [x] Make unrecoverable data errors actionable through an existing platform hook, with tests preserving successful rounded-tail reads.
- [x] Address independent review: reject bad file extents; avoid terminal UI under inherited or callback interrupt locks.

### 5. Verification And Handoff

- [x] Run the full Python, settings, frame-rate, movie and SDK suites; record skips and baseline limitations.
- [x] Cross-compile changed Vita source and complete the full player build with the existing local toolchain.
- [x] Obtain independent review and repair important findings with regression tests.
- [x] Refresh source checksums after final edits.
- [x] Produce a concise report separating demonstrated fixes, remaining hypotheses and physical-Vita acceptance requirements.

### 6. Requested Second Review And Performance Explanation

- [x] Recheck public main/tag identity and obtain independent settings, preparation/CI and I/O reviews.
- [x] Reproduce and reject settings serialization beyond the runtime's 96-assignment limit without discarding owner values.
- [x] Reproduce false completeness with a minimized FST; authenticate the supported FST before trusting its inventory and reject transferable data links.
- [x] Compare v1.0 and v1.1 defaults, distinguish new GPU work from preserved configuration, and leave unmeasured rendering defaults unchanged.
- [x] Reproduce the released frames-in-flight limiter's self-induced slowdown using actual implementation code, then fix cadence accounting with RED/GREEN tests and independent review.
- [x] Complete fresh aggregate verification, Vita rebuild/repack and source checksums after these additional fixes.

### 7. Final Requested Confirmation

- [x] Independently retest the actual frame gate against the released implementation and across additional timing/depth/transition cases.
- [x] Execute the exact host32 SDK, movie CI target, and real EGL rendering tests; close missing test-fixture dependencies without weakening assertions.
- [x] Fix case-different asset-directory preparation and preserve missing-root errors.
- [x] Reproduce and fix startup fallback plan application, derived-variable tracking, conditional timers, same-launch recovery bookkeeping and intentional process-replacement/exit reservations.
- [x] Finish concurrent guard-state verification, final independent review, aggregate tests and rebuilt package. Final source-checksum verification is recorded in the audit.
- [ ] Physical-Vita FPS, cinematic, visual/control and overwrite/fresh-install acceptance. Separate device scope remains required; local tests cannot satisfy this item.

## Review Focus

- Unknown/manual settings survive unrelated saves; legacy preset precedence matches runtime.
- Rejected reads retry, accepted reads are not duplicated, and audio does not advance incorrectly.
- Missing, truncated and malformed assets fail with the offending path; no save or source files change.
- Healthy startup and healthy DVD reads gain no per-file filesystem scan.
- The public binary remains unchanged and is not represented as containing local source fixes.

## Progress

- Initial Python baseline: 40 tests passed; recorder class failed to link its weak `np_time_real_ns` dependency on macOS.
- Initial portable settings baseline: 4,365 checks passed. New legacy-preset tests: 4,394 checks, 23 expected failures before implementation.
- No report's physical-Vita symptom has been reproduced during this task.
- Final Python suite: 52 tests passed. Settings: 4,425 checks passed. FPS: 25 checks passed. Full ARMHF SDK suite with owned read-only assets passed; two real custom movie integrations passed. Renderer normal/sanitized suites passed.
- Independent review corrections: FST extent checks, inherited-IRQ release for terminal error presentation, and assetless CI movie guard. No remaining material review findings.
- Full player build and repack passed. Candidate and runtime hashes, exact artifact path, original-package preservation and device acceptance boundaries are recorded in `docs/COMMUNITY-FIXES-AUDIT-2026-10-26.md`.
- No commit, push, release replacement or device operation performed.
- Requested second review: fixed supported-FST trust, candidate settings assignment overflow and the released frames-in-flight gate's cadence accounting. Fresh Python 54/54, settings 4,431, pipeline gate 12,013 and controller 25 checks passed; SDK and sanitized renderer reruns passed. Vita rebuild/repack and retained-member verification passed. The pipeline rates are synthetic, not physical-Vita performance evidence.
- Final requested confirmation: actual startup/restart/exit and concurrent recovery tracking are fixed, with 134 runtime cases passing under normal, address/undefined and thread sanitizers. Settings remain 4,431/4,431. Exact host32 SDK and Vita-platform-fake suites, movie admission/integration/scheduler, camera 41-check regression and real EGL tests pass. Supported owner extraction validates read-only; directory-case regressions raise Python coverage to 56 tests. Final candidate was rebuilt/repacked and all eight retained public members verified; no hardware or release claim is made.
