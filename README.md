# Mario Strikers Charged Vita

A native PS Vita port of Mario Strikers Charged, with Vita controls, audio
and graphics support.

**The desktop installer is optional.** The game runs on your **PS Vita**.
You can set it up manually from **Windows, macOS (Intel or Apple Silicon),
or Linux** using VitaShell. No computer or desktop app is needed while playing.

**Public preview.** Combined-package licensing and corresponding-source review
are still pending, as is a complete installation test through the desktop app.
Publication is not a claim of third-party approval. See
[Notices](THIRD_PARTY_NOTICES.md) and [Known Issues](KNOWN-ISSUES.md).

Around 30 FPS is what we've been able to achieve in testing so far, though
performance varies by scene. There are still bugs to fix and performance
improvements to make. This build includes the latest intro and stadium-preview
fixes, including the movie audio-buffer fix. Runtime logging is disabled in
this build. LiveArea Settings now opens over the running game and lets you
return without restarting.

## In This Build

- Modern Vita controls, a Classic option and Vita button prompts.
- Seven camera zoom levels, with Auto Zoom and two extra close-up views.
- GPU skinning, renderer optimizations and a packaged startup shader cache.
- Recent fixes for startup delays, loading-screen flicker and stadium-preview
  audio timing.
- Smoother movie pacing without dropping the audio when a picture is skipped.
- In-game LiveArea Settings, with a separate confirmation before restarting.

## Install

### Without Our Desktop App

**Windows, Mac or Linux. No Python or compiling required.**

You need a homebrew-enabled Vita, VitaShell, about 3 GB free, your own
**Wii USA Rev 1 (`R4QE01`)** game dump and a legitimately obtained
`libshacccg.suprx`. The last file is a one-time Vita module, not a PC program.

1. Download [the ready-made VPK](https://github.com/CBOC-Vita-Lab/strikers-charged-vita/releases/download/strikers-settings-20261019/Strikers-settings-20261019.vpk), or the [complete install ZIP](https://github.com/CBOC-Vita-Lab/strikers-charged-vita/releases/download/strikers-settings-20261019/Strikers-settings-20261019-install.zip) with guides and source. Keep the VPK intact.
2. [Extract your ISO/RVZ in Dolphin](PREPARE-GAME.md#dolphin-no-python-needed). Already have the complete `sys/` and `files/` folders? Skip extraction. Dolphin is only used to extract files, not to run the game on Vita.
3. Copy `sys/` and `files/` into `ux0:data/mscharged/game/`, and the VPK to `ux0:VPK/`. Use [USB, FTP or an already configured memory card](INSTALL.md#choose-a-transfer-method).
4. If missing, place your `libshacccg.suprx` at `ur0:data/libshacccg.suprx`. End the transfer, install the VPK in VitaShell, then launch the Strikers bubble.

**[Full manual guide](INSTALL.md)** includes exact folders, transfer steps and
the shader-module copy step. [Start Here](START-HERE.txt) is the short checklist.
The VPK includes the player, startup shaders and default artwork; no separate
`eboot.bin`, shader-cache replacement or desktop installer is needed.

## Easy Install (Optional)

[CBOC Vita Installer](https://github.com/CBOC-Vita-Lab/vita-installer/releases/latest)
does the copying and installing for you, on Mac (macOS 10.13+), Windows (7+) or
Linux. Open it, press SELECT in VitaShell, click **Get** next to Mario Strikers
Charged, choose your own disc (ISO, RVZ, WBFS or an extracted folder) and click
**Install**. Saves and settings are kept. [Easy install guide](DESKTOP-INSTALL.md).

## Already Installed?

Back up your saves and settings, then install only the matching
`Strikers-settings-20261019.vpk` in VitaShell. Keep your existing game data.
No desktop app or repeat extraction is needed. See [Update and Back Up](INSTALL.md#update-and-back-up).

The executable matches the recorded physical-Vita update; a full fresh desktop
installation still needs its own hardware check. Game data and the proprietary
shader compiler are not included. [Known Issues](KNOWN-ISSUES.md) and
[Source Record](SOURCE.md) hold the testing and release details.

[Controls](CONTROLS.md) | [Building](BUILDING.md) | [Source Record](SOURCE.md)
| [Technical Notes](TECHNICAL.md) | [Included Files](ASSETS.md)
| [Licenses](THIRD_PARTY_NOTICES.md) | [Changes](CHANGELOG.md) | [Credits](CREDITS.md)

Upstream contributors retain their credit and rights; see [Credits](CREDITS.md).

[Store listing details and screenshots](store/README.md).
