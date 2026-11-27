# Strikers Hardware Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Compare public 1.0, public 1.1 and the complete 1.1.1 fixes on the physical Vita without modifying the original installation.

**Architecture:** One isolated test identity and private writable tree, replaced sequentially by equivalently isolated baseline/candidate builds. The USB-testing chat supplies reviewed transport and explicitly hands off device control; raw evidence remains local.

**Tech Stack:** Python unittest, VitaSDK/Clang cross-build, VitaGL, existing input/diagnostic harness, USB transport provided by the coordinating chat.

**Spec:** `docs/HARDWARE-VALIDATION-SPEC-2026-10-26.md`

## Global Constraints

- Protect `MSCN00003` and all of `ux0:data/mscharged` against writes.
- Only one test app/package on the Vita; no device operations before explicit control handoff.
- No production release metadata, public assets, tags, or GitHub releases changed before device acceptance.
- No proprietary assets, saves, or raw identifying evidence in public Git.
- Use `NP_PLAYER=1`, `LAB=0`, `NP_LOGGING=0`; no virtual timing model.
- All variants must have identical isolation/profile flags and independent fresh build roots, preserving historical runtime/drivers; use common external observation where 1.0 lacks the newer recorder.

## Review Focus

- Wrong SFO inherited by repacking must fail package verification before installation.
- Config rereads and legacy save-import behavior must not escape the private writable root.
- VitaGL's global FFP cache must not silently modify shared state.
- Script EOF is not physical-input handoff; manual input acceptance must run without a script.
- Recorder corruption/overflow and submit-only timing must never count as displayed-FPS acceptance.

### Task 1: Safe Isolation Tooling

**Files:** Create `tools/stage-hardware-test.py`; test `tests/test_hardware_stage.py`.

**Interfaces:** A fail-closed deterministic staging function takes a source snapshot and new destination, produces isolated source plus a manifest/diff. A package verifier parses SFO and executable provenance before allowing a test package.

- [ ] Write and run failing tests for private roots, read-only disc exception, disabled save import, unsafe config overrides, dependency caches, wrong/malformed SFO and protected destination rejection.
- [ ] Implement staging without changing production runtime sources in place; apply the same transformation to baseline and candidate.
- [ ] Run focused tests and existing Python tests; independent review before building/installing.

### Task 2: Reproducible Baseline And Candidate

**Files:** Generated only under `build/hardware-validation/`; record commands/hashes in a local evidence ledger.

**Interfaces:** Task 1 isolated source snapshots and verified test packages; existing seed scripts and owner extraction as read-only inputs.

- [x] Export tags `v1.0`/`v1.1` and snapshot the current fixes; preserve original public VPK hashes.
- [x] Build all three with the same isolation/toolchain/profile flags, each version's own historical dependencies/cache, distinct fresh outputs, and provisional title ID `CDXMP0001`; installation remains gated on fresh inventory verifying its scope.
- [x] Verify ZIP members, parsed SFO/content ID, ELF/eboot provenance, changed source manifests, private roots, and absent proprietary payloads.
- [x] Prepare test config/save/script fixtures and a precise case matrix, including settings, I/O faults, movies, Sand Tomb, Mega Saves, and repeated ordinary play. The separately compiled private-disc fault variant remains a Task 3 execution prerequisite, not one of the three default comparison packages.

### Task 3: Controlled Device Comparison

**Files:** Local backups, inventory manifests, raw logs/captures, and case ledger under ignored build storage.

**Interfaces:** Reviewed transport/client and explicit device-control handoff from USB chat; verified Task 2 packages.

- [ ] Inventory and hash original/test state read-only; back up protected app/settings/saves/art/cache before writing.
- [ ] Install only the fixed test package first as requested; read back title ID, package identity, and protected hashes before launch.
- [ ] Exercise fixes and baseline A-B-B-A comparisons, replacing the same test app, with matched settings/cache/clocks and capture-off/on samples.
- [ ] Investigate each failure using a reproduced failing case; apply tested narrow fixes, rebuild, and repeat affected/regression cases.
- [ ] Check saved progress, original hashes, valid telemetry, ordinary-play visuals/audio, and explicit manual-input acceptance; record anything untested or transport-limited.

### Task 4: Acceptance And Conditional Release

**Files:** Hardware validation report; release files only after the acceptance gate opens.

**Interfaces:** Valid physical evidence from Task 3 and user acceptance.

- [ ] Summarize measured results separately from local models and blocked cases; obtain user acceptance of the named `1.1.1` candidate.
- [ ] Only then finish source/checksum review, release build verification, commit/tag/push and release attachment under the approved version.
- [ ] Release packaging must update the stale production SFO version label rather than inheriting `01.00` unchanged; verify an unambiguous legal Vita version label and document its mapping to release `1.1.1` while preserving artwork/cache.
- [ ] Verify published artifacts and hand back device control; otherwise leave the candidate unreleased and explain the specific remaining gate.
