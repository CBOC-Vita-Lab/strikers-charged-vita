# Changes

## 2026-10-23: Mega Saves, Touch And Zoom Update

Gameplay and controls:
- Faster Mega Saves: zero-alpha effect sprites are skipped, and faint additive
  haze is drawn one in four at four times the strength during Mega Strikes.
  `MSC_FX_THIN=0` and `MSC_FX_ZERO=0` in `np-player.txt` restore the old drawing.
- 60 fps by default where it holds: Settings > Frame Rate is now AUTO by
  default (30 FPS is still there). AUTO also watches the graphics chip now,
  so it does not try 60 in a scene the GPU cannot draw that fast.
- Dynamic resolution: the renderer measures the graphics chip's time every
  frame and adjusts the picture to stay inside the frame budget (60 or 30).
  When it is short it first draws the large blur and glow effects at a
  smaller size, then the scene at 93 % and at most 86 % of each side; menus
  and matches never go lower. With time to spare it draws the scene larger
  than the game's own size (up to 1.2 times, 768x544), for a sharper
  picture. Mega Strike scenes start at 70 % and may go down to 50 %, with
  stronger haze thinning at the bottom. Settings: `MSC_DYNRES=off`,
  `MSC_DYNRES_MAX=1.0..1.2`, `MSC_DYNRES_MIN=0.75..1.0`,
  `MSC_MEGA_RENDER=off|WxH` in `np-player.txt`.
- Sharper picture everywhere: the picture is now scaled to the screen with a
  light sharpening filter (Settings > Sharpness: SMOOTH, the new default). It
  sharpens more when the scene is drawn smaller. OFF restores the plain
  scaling; CRISP and CRISP+ are still there.
- Mega Save touch aim uses the position sampled for that press.
- Optional rear-touch aim and Mega stick speed (2x-4x) in LiveArea Settings.
- Tap anywhere on the title screen to start. A finger already down does not
  count, and the tap does not press anything in the main menu.
- Short front-panel taps between reads are no longer lost.
- Button taps are no longer swallowed by the debug poll or replayed after a
  system dialog.
- Options > Visual shows all seven zoom levels: 1-5, or 3-7 from level 4 up.
- From zoom level 4 up, the CPU judges "on screen" from about the TV-default
  view, so close cameras no longer make the AI play worse.

Performance and reliability:
- Shader cache: entries are written whole and replaced when invalid. Compile
  and link failures are bounded, and a uniform-buffer leak is fixed.
- Graphics memory recovery waits for the collector instead of sleeping.
- DXT1 mip chains upload without repeated storage growth.
- Background threads wait without 500 us polling.
- Characters hidden during Mega Strikes are not prepared for drawing.

Diagnostics and logging:
- `MSC_VITA_DISPLAY_BUFFERS=2` (test switch): double buffering, for a console
  A/B of input latency. [Console tests](docs/CONSOLE-TESTS.md) lists the
  measurements that decide the next optimisations.
- vitaGL is built with `-ffile-prefix-map`, so no local paths end up in the
  executable.
- Development builds write the log from a background thread, so card writes
  no longer stall the game or change the timings being measured.
- Performance recorder: its memory is allocated only when it is turned on, the
  bank title follows the build, the CRC table is built once, and GPU-call
  timing is an explicit build switch (`NP_DIAG_GPU=1`).
- New `tools/diag/perf-report.py` reads the recorder's bank files (display
  cadence, handover to display, back-end and GPU-call times, tasks) and flags
  damaged or incomplete banks. A host round-trip test checks it against the
  real recorder. Guide: `docs/DIAGNOSTICS.md`.

Repository:
- Stable asset names (`Strikers-Charged-Vita.vpk`, `-install.zip`), so
  "latest" download links keep working.
- Developer docs moved to `docs/`, and a reproducible repack tool was added
  (`tools/repack-release.py`).
- Release-metadata tests, run with the host tests in GitHub Actions.

## 2026-10-19: LiveArea Settings Update

- Open Settings from LiveArea while the game is running, then return to play.
- Save changes for the next launch, or restart only after confirmation.
- Pause game timing and audio while settings are open.
- Retain the movie fixes and disabled runtime logging.
- Include matching source, VPK and ZIP. Desktop setup requires installer 0.1.10+.

## 2026-10-19: Quiet Update

- Disabled runtime logging, console output and startup diagnostic probes.
- Kept the intro, stadium-preview and movie-audio fixes, plus normal save I/O.
- Updated the matching source, VPK, install ZIP and checksums.
- Requires CBOC Vita Installer 0.1.8 or newer for desktop installation.

## 2026-10-19: Movie Update

- Improved intro and stadium-preview pacing while keeping the movie audio.
- Fixed audio-buffer access during playback and retries after delayed reads.
- Added regression tests for movie scheduling, audio and decoder-buffer reuse.
- Included the matching production VPK with generic art and existing shaders.
  Its executable matches the verified main-app update on a physical Vita.

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
