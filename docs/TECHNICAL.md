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

The lab tools include scripted runs, diagnostics and deployment helpers.
They are historical developer tools, not ordinary install instructions.
Host or emulator timing is not physical-Vita performance evidence.

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
emission and timing are unchanged.
