# Changes

## 2026-10-18: Launch Preparation

- Made the desktop installer the main quick-start route, with manual setup
  still available.
- Clarified that ZIPs and complete extracted release folders both work.
- Kept the older private preview clearly separate from the intro/stadium
  update still undergoing hardware tests.
- Shortened the project description and clarified the remaining performance work.

## 2026-10-18: Copy-And-Install Setup

- Made the ready-made VPK and copy-folder steps the main install path.
- Added Dolphin extraction instructions; Python is an optional helper only.
- Used generic app art and the built-in loading screen instead of personal
  fan art. Added notices without changing the installed executable or shaders.

## 2026-10-18: Installed Build Preserved

- Kept the installed executable, artwork, loading image and shaders unchanged.
- Kept source-build validation outputs outside the install bundle.
- Added missing font and linked-library notices to the source.

## 2026-10-18: Dependency Setup

- Recovered the library-build recipe and pinned its external source inputs.
- Added `BUILD-DEPS.py` to fetch and build them without changing VitaSDK.
- Kept the original game, renderer and installed runtime unchanged.

## 2026-10-18: Simpler Setup

- Added one-command game preparation with a verified extractor download,
  automatic disc checks and no replacement of existing output folders.
- Shortened the quick start and moved detailed release notes out of the main flow.
- Included preparation tools, source and linked guides in the local bundle.

## 2026-10-18: Source And Documentation

- Added the recovered `a8d4ad6` native source, renderer and patched vitaGL files.
- Added build provenance, dependency gaps, technical notes and license texts.
- Kept the simple VPK-plus-own-data install path and backup instructions.
- No runtime rebuild, console install, public release or visibility change.

The selected build includes the camera/control, startup, skinning and audio
changes summarized in [README](README.md). See [Known Issues](KNOWN-ISSUES.md)
for the remaining hardware checks.
