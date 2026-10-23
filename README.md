# Mario Strikers Charged for PS Vita

A native PS Vita port of **Mario Strikers Charged** (Wii, 2007). The game's
reconstructed C++ source is compiled straight to the Vita's ARM CPU, so there
is no emulator and no PowerPC recompiler. A purpose-built renderer turns the
game's Wii graphics calls into vitaGL. Vita controls, touch, audio, saves and
LiveArea settings are built in.

> **Fully vibecoded.** This port was written by AI coding agents (Claude,
> with help from other assistants) working on a real console, and directed and
> play-tested by a human. Upstream projects keep their credit: see
> [Credits](CREDITS.md).

**Public preview.** Most scenes run at about 30 FPS on hardware. Some menus and
effect-heavy moments are still slower. Read [Known Issues](KNOWN-ISSUES.md)
before you install. You need your own game dump. No game data is included.

![Gameplay on a PS Vita](store/screenshots/gameplay-vita.png)

## What's New (2026-10-23)

- **Faster Mega Saves.** The Mega Strike save scene draws far fewer invisible
  or near-invisible effect sprites and renders at a lower internal size that
  drops further if it falls below 30 fps.
- **Tap anywhere to start** on the title screen.
- **All seven zoom levels on the Options slider.** It shows levels 1-5, or 3-7
  once you choose level 4 or higher.
- **Smarter CPU at close zoom.** The AI sees about as much of the pitch as at
  the original TV camera, even when your view is zoomed in.
- Quick screen taps are no longer missed, and button taps no longer get lost
  or replayed.
- Shader-cache, memory-recovery and texture-upload fixes. Optional rear-touch
  aim and a faster Mega stick speed are in Settings.

Full list: [Changes](CHANGELOG.md).

## Install

You need:

- a homebrew-enabled Vita with VitaShell and about 3 GB free;
- your own **Wii USA Rev 1 (`R4QE01`)** game dump;
- a legitimately obtained `libshacccg.suprx` (the Vita's shader compiler module).

Steps:

1. Download **[Strikers-Charged-Vita.vpk](https://github.com/CBOC-Vita-Lab/strikers-charged-vita/releases/latest/download/Strikers-Charged-Vita.vpk)**.
   For the VPK plus guides, checksums and source, get the
   [install ZIP](https://github.com/CBOC-Vita-Lab/strikers-charged-vita/releases/latest/download/Strikers-Charged-Vita-install.zip) instead.
2. [Extract your ISO/RVZ with Dolphin](PREPARE-GAME.md#dolphin-no-python-needed)
   to get the `sys/` and `files/` folders. Dolphin is only used to extract.
3. Copy `sys/` and `files/` to `ux0:data/mscharged/game/`, and the VPK to
   `ux0:VPK/` ([transfer methods](INSTALL.md#choose-a-transfer-method)).
4. If it is missing, put `libshacccg.suprx` at `ur0:data/libshacccg.suprx`.
   Then install the VPK in VitaShell and launch the bubble.

**Updating?** Install only the new VPK. Your game files, saves and settings in
`ux0:data/mscharged/` stay as they are. See
[Update and Back Up](INSTALL.md#update-and-back-up).

More detail: [Full install guide](INSTALL.md) · [Short checklist](START-HERE.txt) ·
[Optional desktop installer](DESKTOP-INSTALL.md) · [Check the download](CHECK-DOWNLOAD.md)

## Playing

The default **Modern** layout:

- Cross passes or switches player.
- Circle shoots or slide-tackles.
- Square lobs or big-hits.
- Triangle uses an item.

A **Classic** option is also available. Mega Saves can use touch or either
stick. Open **Settings** from the LiveArea while the game runs to change
controls, picture and performance options, then return to your match. Saved
changes apply at the next launch, or choose Save and Restart. Camera zoom is
in the game's own Options > Visual. See [Controls](CONTROLS.md).

## For Developers

| | |
|---|---|
| [Building](docs/BUILDING.md) | Toolchain, pinned dependencies, build and packaging |
| [Technical notes](docs/TECHNICAL.md) | Source layout, data paths, runtime switches |
| [Source record](docs/SOURCE.md) | What this release was built from, with hashes |
| [Diagnostics](docs/DIAGNOSTICS.md) | Log, performance recorder and report tool |
| [Included files](docs/ASSETS.md) | Package contents and artwork |
| [Licenses](THIRD_PARTY_NOTICES.md) | Third-party notices |

Source layout: `native/` holds the game code, the Wii SDK replacement and the
Vita platform layer. `renderer/` is the GX→GL renderer, and
`dependencies/vitaGL/` is the patched vitaGL. Host tests run on every push
(see `.github/workflows/`).

## Status And Thanks

This is an unofficial fan project. It is not affiliated with or endorsed by
Nintendo or Sony. Mario Strikers Charged is © Nintendo. Built on the
[mscharged-decomp](https://github.com/yannicksuter/mscharged-decomp)
decompilation, [vitaGL](https://github.com/Rinnegatamante/vitaGL), VitaSDK and
others listed in [Credits](CREDITS.md) and the [notices](THIRD_PARTY_NOTICES.md).
Combined-package licensing review is still pending.
