# Mario Strikers Charged Vita

A native PS Vita port of Mario Strikers Charged, with Vita controls, audio
and graphics support.

Around 30 FPS is what we've been able to achieve in testing so far, though
performance varies by scene. There are still bugs to fix and performance
improvements to make. The new intro and stadium-preview changes are being
checked before the launch package is selected.

## In This Build

- Modern Vita controls, a Classic option and Vita button prompts.
- Seven camera zoom levels, with Auto Zoom and two extra close-up views.
- GPU skinning, renderer optimizations and a packaged startup shader cache.
- Recent fixes for startup delays, loading-screen flicker and stadium-preview
  audio timing.

## Install

The simplest route uses [CBOC Vita Installer](https://github.com/CBOC-Vita-Lab/vita-installer/releases):

1. Connect your Vita, then select the matching release ZIP or its extracted folder.
2. Select your own **Wii USA Rev 1 (`R4QE01`)** ISO/RVZ or complete game extraction.
3. Add any missing required file when prompted, choose your artwork, and install.

[Desktop Install](DESKTOP-INSTALL.md) lists the requirements. No separate Python,
Dolphin or compiling is needed for that route. Prefer manual setup? Install the
VPK with VitaShell and copy your own game folders using [Install](INSTALL.md).

[Start Here](START-HERE.txt) is the short checklist. [Install](INSTALL.md)
has the requirements, folder map and update steps. The VPK already carries
its startup shaders; no manual `eboot.bin` or shader replacement is needed.

The current `a8d4ad6` download is an older private preview, not the upcoming
movie-update launch build. Do not mistake it for the latest test candidate.
Game data and the proprietary shader compiler are not included. [Known Issues](KNOWN-ISSUES.md) and
[Source Record](SOURCE.md) hold the testing and release details.

[Controls](CONTROLS.md) | [Building](BUILDING.md) | [Source Record](SOURCE.md)
| [Technical Notes](TECHNICAL.md) | [Included Files](ASSETS.md)
| [Licenses](THIRD_PARTY_NOTICES.md) | [Changes](CHANGELOG.md) | [Credits](CREDITS.md)

Upstream contributors retain their credit and rights; see [Credits](CREDITS.md).
