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
| `MSC_FX_THIN=0` | Draw Mega Strike haze particles as the original did (no thinning) |
| `MSC_FX_THIN=2` | Stronger thinning (up to alpha 24, fading out by 48), for console A/B tests |
| `MSC_FX_ZERO=0` | Also rasterise fully transparent particles |
| `MSC_TOUCH_LATCH=0` | Do not keep front-panel taps that fall between two reads |
| `MSC_CAMERA_VIS=screen` | The AI's on-screen test uses the real screen at every zoom level |
| `MSC_MEGA_AIM=rear`, `MSC_MEGA_POINTER_SPEED=2..4` | Mega Save rear-touch aim and stick speed |
| `MSC_TOUCH_DIAG=1` | In-memory performance recorder (development; off by default) |
| `NP_TOUCH_POINTER=0` | No front-panel presses at all, title tap included |

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
