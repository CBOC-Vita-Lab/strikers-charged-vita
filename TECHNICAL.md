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
is not a supported replacement. See [Install](INSTALL.md) for exact paths.

The lab tools include scripted runs, diagnostics and deployment helpers.
They are historical developer tools, not ordinary install instructions.
Host or emulator timing is not physical-Vita performance evidence.
