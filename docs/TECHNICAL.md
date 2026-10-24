# Technical Notes

This is the native ARM port. It is not the older PowerPC-recompiled runtime.

- `native/src/` and `native/include/`: reconstructed game code with port changes.
- `native/platform/sdk/`: host replacements for console services.
- `native/platform/gx/`: bridge from the game's GX calls to `renderer/`.
- `native/platform/vita/`: input, audio, memory, packaging and player settings.
- `dependencies/vitaGL/`: recovered patched graphics implementation.

The selected player executable uses ThinLTO. Renderer and platform code must
agree on enum, char and wchar sizes; use the Makefile's ABI flags rather
than compiling individual files with guessed defaults.

The player uses `ux0:data/mscharged/`; extracted game data lives in `game/`.
Preserve `np-data/` and existing NAND/save files, plus `np-player.txt`.
The startup shader cache is build-specific. A cache from another renderer
is not a supported replacement. See [Install](../INSTALL.md) for exact paths.

`native/platform/vita/lab/` holds developer tools for scripted test runs and
diagnostics. They are not needed to install or play. Desktop or emulator
timing is not a measure of Vita performance.

## Runtime Switches

Lines in `ux0:data/mscharged/np-player.txt`. LiveArea Settings writes the Mega
Save ones; add the others by hand. These are the switches added for the
2026-10-23 release:

| Line | Effect |
| --- | --- |
| `MSC_FX_LOWRES=0` | Draw glowing effect sprites at full size (default: Mega Strikes and heavy effects at a quarter of the pixels; `4`: a sixteenth; `always`: every effect, for tests) |
| `MSC_FX_THIN=0` | Never thin the Mega Strike haze (default: only at the smallest Mega Strike picture; `1`: always in Mega Strikes) |
| `MSC_VITA_ANISO=3` | Experimental anisotropic filtering of trilinear textures (Settings > Distant Textures). Untested on a console |
| `MSC_VITA_UPSCALE=off` | Plain scaling to the screen, no sharpening (default `smooth`: bilinear plus a light unsharp mask, amount `MSC_VITA_SHARPEN`, default 0.5, and 0.7-0.8 in Mega Strike scenes). `sharp`, `sharp+` and `linear` remain |
| `MSC_FPS_MEGA=auto` | Let Mega Strikes try 60 like other scenes (default: they stay at 30 with the full-size picture) |
| `MSC_VITA_QUEUE=off` | No GPU queue limit (default `auto`: at most 2 unfinished frames on the GPU, 1 while the GPU needs 78 % or more of the frame time) |
| `MSC_FPS=30` | A steady 30 fps (Settings > Frame Rate: 30 FPS). The default is `auto`: 60 where every CPU core and the GPU hold it |
| `MSC_DYNRES=off` | No dynamic resolution: the game's own picture size everywhere (also turns the GPU timers off) |
| `MSC_DYNRES_MAX=1.0` | Do not draw larger than the game's own size when the GPU has time to spare (default up to 1.2 per side) |
| `MSC_DYNRES_MIN=0.93` | The smallest picture in menus and matches (default 0.86 per side; 0.75 to 1.0) |
| `MSC_MEGA_RENDER=off` | Mega Strike scenes use the ordinary floor (default: they may go down to 50 % if the GPU is still short) |
| `MSC_MEGA_RENDER=WxH` | One fixed internal size during Mega Strike scenes, e.g. `448x320` (160x120 to 960x544). Ignored when `MSC_VITA_RENDER` is set |
| `MSC_VITA_RT_TIMERS=0` | GPU timers off (the dynamic resolution then keeps the game's size, apart from the Mega Strike start) |
| `MSC_FX_THIN=2` | Stronger thinning (up to alpha 24, fading out by 48), for console A/B tests |
| `MSC_FX_ZERO=0` | Also rasterise fully transparent particles |
| `MSC_TOUCH_LATCH=0` | Do not keep front-panel taps that fall between two reads |
| `MSC_CAMERA_VIS=screen` | The AI's on-screen test uses the real screen at every zoom level |
| `MSC_MEGA_AIM=rear`, `MSC_MEGA_POINTER_SPEED=2..4` | Mega Save rear-touch aim and stick speed |
| `MSC_TOUCH_DIAG=1` | In-memory performance recorder (development; off by default) |
| `NP_TOUCH_POINTER=0` | No front-panel presses at all, title tap included |

## Dynamic Resolution And Frame Rate

`renderer/src/vgx_backend_gl.cpp` ("DYNRES") reads the GPU time of each finished
frame from vitaGL's GPU timers (`vglRtLastGpuFrame`: one fragment notification
per render scene, a watcher thread on core 2). It moves along a ladder of
picture levels so that the GPU stays inside the frame budget of the frame rate
in force (16.7 ms at 60, 33.4 ms at 30):

| Levels | Scene scale | Large passes | When |
| --- | --- | --- | --- |
| 0-2 | 1.20, 1.12, 1.06 | 1.0 | The GPU has room (under 72 % of the budget at the next level for 45 frames) |
| 3 | 1.0 | 1.0 | The game's own size (the start) |
| 4-5 | 1.0 | 0.75, 0.5 | The first saving: blur and glow chains, full-screen filters |
| 6-7 | 0.93, 0.86 | 0.5 | The floor of menus and matches |
| 8-12 | 0.78 to 0.50 | 0.5 | Mega Strike scenes only (they start at level 5: the scene at full size, the effect sprites at a quarter of the pixels) |

"Large passes" are offscreen passes of at least an eighth of the frame buffer
that are not drawn into an atlas. Over 90 % of the budget it jumps straight to
the best level predicted to fit in 82 %. Copy textures and the frame buffer keep
their picture, scaled, when their size changes, so a change shows no flash.
`VgxGpuMinMs` gives the frame-rate controller (`native/platform/compat/np_fps_ctl.h`)
the GPU time predicted at the scene's floor: AUTO does not go to 60 when that is
over 85 % of a refresh, and goes back to 30 when it stays over 97 % for 8 frames.
The host's sharpening follows the scale (`VgxRenderScale`).

Effect sprites (FX): the game marks its additive sprite meshes (`np/fx.h`;
`ParticleSystem` and `BuildParticleQuads` in `main.cpp`, the path the game
actually uses), `glxSend` wraps their draws in an `EXT_FX` record, and the
back end draws a run of them into a half-size target that starts with the
scene's depth (a depth-copy pass) and adds it to the frame buffer in one
full-screen pass. Runs under 400 vertices outside Mega Strikes are drawn as
before, since a run costs two scene changes and about 1.25 screens of pixels.

On the desktop harness, `MSC_DYNRES_FAKE=a,b` simulates a GPU time and
`MSC_DYNRES_CYCLE=N` changes the level every N frames (a stress test of the
size changes).

## Mega Saves Rendering

The save scene is graphics-bound by blending. It draws about 40 screens' worth
of overlapping additive particle quads per frame, and one faint layer (alpha
0-11 of 255) covers about 28 of them. `ParticleSystem::RenderAllParticles`
writes a zero-area quad for any particle whose alpha is 0, because such
particles fail the alpha test everywhere. While the Mega Strike background is
up, it keeps one in four faint additive particles at four times the alpha.
The choice follows the particle's pool slot, so a particle does not flicker.
Between alpha 12 and 32 the thinning fades out to the original. Simulation,
emission and timing are unchanged. Since version 1.1 this thinning applies
only at the smallest Mega Strike picture size; the low-resolution effects pass
below does the heavy lifting.

## Version 1.1 Changes

The full engineering list behind the [1.1 release notes](../CHANGELOG.md).

## 2026-10-23: Mega Saves, Touch And Zoom Update

### Gameplay, rendering and controls
- Faster, sharper Mega Strikes: glowing effect sprites (the Mega Strike haze,
  item and super-ability glows) are drawn at a quarter of the pixels into a
  separate image, still hidden behind what is in front of them, and added to
  the scene in one pass. The scene itself stays at full size. Used in Mega
  Strikes, and for heavy effects elsewhere when the graphics chip is short.
  `MSC_FX_LOWRES=0` turns it off. Fully transparent sprites are skipped. The
  haze thinning of earlier builds now only applies at the smallest Mega Strike
  picture (it was never reached before: the game draws its sprites through a
  second path, which now has it as well).
- 60 fps by default where it holds: Settings > Frame Rate is now AUTO by
  default (30 FPS is still there). AUTO also watches the graphics chip now,
  so it does not try 60 in a scene the GPU cannot draw that fast. Mega
  Strikes stay at 30 with the full-size picture (`MSC_FPS_MEGA=auto` lets
  them try 60).
- Less input delay in slow scenes: when the graphics chip needs most of the
  frame time, only one frame may wait on it instead of two (the change made
  for Super Mario Galaxy). `MSC_VITA_QUEUE=off|1|2` overrides it.
- Dynamic resolution: the renderer measures the graphics chip's time every
  frame and adjusts the picture to stay inside the frame budget (60 or 30).
  When it is short it first draws the large blur and glow effects at a
  smaller size, then the scene at 93 % and at most 86 % of each side; menus
  and matches never go lower. With time to spare it draws the scene larger
  than the game's own size (up to 1.2 times, 768x544), for a sharper
  picture. Mega Strike scenes may go down to 50 % if still short, with
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

### Performance and reliability
- Shader cache: entries are written whole and replaced when invalid. Compile
  and link failures are bounded, and a uniform-buffer leak is fixed.
- Graphics memory recovery waits for the collector instead of sleeping.
- DXT1 mip chains upload without repeated storage growth.
- Background threads wait without 500 us polling.
- Characters hidden during Mega Strikes are not prepared for drawing.

### Diagnostics
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

### Repository
- Stable asset names (`Strikers-Charged-Vita.vpk`, `-install.zip`), so
  "latest" download links keep working.
- Developer docs moved to `docs/`, and a reproducible repack tool was added
  (`tools/repack-release.py`).
- Release-metadata tests, run with the host tests in GitHub Actions.
- Developer files moved out of the top folder: `tools/build-deps.py`,
  `tools/check-files.py`, `docs/RELEASE.json`, `docs/SOURCE-MANIFEST.json`,
  `docs/CHECK-DOWNLOAD.md` and `store/artwork.json`.

### Console fixes after the first hardware test
- The framebuffer-object pool kept every pass size the dynamic resolution had
  used, with its colour and depth textures, for the whole session; after a few
  level changes the video memory ran out and creating a target crashed the GPU
  set-up (`sceGxmBeginScene`, at the start of a match). Pool objects unused for
  60 presents are now deleted, and the controller only steps up when vitaGL's
  VRAM pool has room: 32 MiB free for levels above the game's own size, 16 MiB
  for any step up, and below 12 MiB a larger picture drops back to native.
  `MSC_DYNRES_VRAM=KiB` simulates the free memory on the desktop.
- Every fresh run-time shader compile failed on the console ("fatal internal
  error on line -1", Cg and GLSL alike). The shader-cache hardening in the
  patched vitaGL (`custom_shaders.c`, `compile_shader`) passed vitaShaRK a
  size of 0, and vitaShaRK reads that argument as the length of the source
  text, so the compiler saw an empty file. Only packaged or cached shaders
  worked, so the sharpening fell back to the plain picture and the
  low-resolution effects to full size. The size now goes in as the source
  length. The sharpening shader is also written one sample per statement.

- The lighter effects never ran on the console: vitaGL has no depth textures
  (no `GL_OES_depth_texture`, colour attachments only), so the effects pass
  had no scene depth and stayed off, and Mega Strikes started at 70 % size.
  The patched vitaGL now has `vglStrikersBindFramebufferDepth`, which shows a
  framebuffer's stored depth buffer (linear DF32M, rows aligned to 32 samples)
  as the bound texture; the effects pass reads the EFB's depth through it.
  `MSC_FX_VITA_DEPTH=0` turns that path off.
- Size changes are spaced at least 20 frames apart (the start and end of a
  Mega Strike excepted), need the free GPU memory for the new targets plus
  8 MiB (vitaGL's VRAM and RAM pools together), and are followed by a quick
  trim of the framebuffer pool. If the console still cannot allocate the new
  render target, the old one is kept and the level goes back.
- Copy textures that keep their picture over a size change fall back to the
  plain resize when there is no memory for a second copy. Deleted textures are
  detached from pooled framebuffer objects first, so vitaGL frees them at once.
- The pool is never compacted while a pass is open; a frame-rate interval of
  0 (no policy yet) uses the 30 fps budget; with a fixed render size
  (`MSC_VITA_RENDER`) Mega Strikes are still held at 30.
- Second console test: in a Mega Save the GPU time stayed at the frame budget
  while the picture went down to 0.50 (the effects are already at a quarter of
  the pixels). A step down is now judged after about 20 measured frames and
  undone if the GPU time fell by less than 8 %; the scene then stays at that
  level (until the Mega Strike ends, or 900 frames in ordinary scenes). Mega
  Strikes start with the scene at the game's own size when the effects pass
  is available, also from a match's 0.86.
- Mega Save stick speed: 3x by default (`MSC_MEGA_POINTER_SPEED=1` is the
  original speed).
- The package's shader pack and warm-up list include the shaders the console
  compiled in a first match (768 programs' shaders, the warm-up list of that
  console), so the first goal replay at close zoom no longer compiles.
