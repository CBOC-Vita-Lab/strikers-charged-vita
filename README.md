# Mario Strikers Charged Vita

A native PS Vita port of Mario Strikers Charged, with Vita controls, audio
and graphics support.

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

Download **[Strikers-settings-20261019-install.zip](https://github.com/CBOC-Vita-Lab/strikers-charged-vita/releases/download/strikers-settings-20261019/Strikers-settings-20261019-install.zip)**.
The matching VPK, shaders, generic artwork, source and guides are available
individually in this repository. **Code > Download ZIP**, then extracting the
whole folder, provides the complete repository files.

You need a homebrew-enabled Vita, VitaShell, your own **Wii USA Rev 1
(`R4QE01`)** game and a legitimately obtained `libshacccg.suprx`.
That last item is a one-time Vita module, not a desktop program.

For **Apple Silicon Mac**, download
[CBOC Vita Installer 0.1.10](https://github.com/CBOC-Vita-Lab/vita-installer/releases/tag/v0.1.10):

1. Connect your Vita, then paste this repository's URL or select the release ZIP/folder.
2. Select your own **Wii USA Rev 1 (`R4QE01`)** ISO/RVZ or complete game extraction.
3. Add any missing required file when prompted, choose your artwork, and install.

[Desktop Install](DESKTOP-INSTALL.md) has connection and recovery details.
No separate Python, Dolphin or compiling is needed for that route.

Prefer to copy files yourself? [Manual installation](INSTALL.md) covers
VitaShell, exact folders and backups on any computer. [Game preparation](PREPARE-GAME.md)
explains disc extraction; [Technical notes](TECHNICAL.md) and [Building](BUILDING.md)
cover the source. Intel Mac and Windows desktop packages remain validation
candidates, not accepted player downloads.

[Start Here](START-HERE.txt) is the short checklist. [Install](INSTALL.md)
has the requirements, folder map and update steps. The VPK already carries
its startup shaders; no manual `eboot.bin` or shader replacement is needed.

Already installed? Back up your saves and install only
`Strikers-settings-20261019.vpk`; there is no need to recopy unchanged game data.
Installer 0.1.10 or newer recognizes this package. The executable matches the approved update
installed on a physical Vita; the full desktop installation still needs its
own hardware check. The repository and current release downloads are public.
Game data and the proprietary shader compiler are not included. [Known Issues](KNOWN-ISSUES.md) and
[Source Record](SOURCE.md) hold the testing and release details.

[Controls](CONTROLS.md) | [Building](BUILDING.md) | [Source Record](SOURCE.md)
| [Technical Notes](TECHNICAL.md) | [Included Files](ASSETS.md)
| [Licenses](THIRD_PARTY_NOTICES.md) | [Changes](CHANGELOG.md) | [Credits](CREDITS.md)

Upstream contributors retain their credit and rights; see [Credits](CREDITS.md).

[Store listing details and screenshots](store/README.md).
