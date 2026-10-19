# Mario Strikers Charged Vita

A native PS Vita port of Mario Strikers Charged, with Vita controls, audio
and graphics support.

Around 30 FPS is what we've been able to achieve in testing so far, though
performance varies by scene. There are still bugs to fix and performance
improvements to make. This build includes the latest intro and stadium-preview
fixes, including the movie audio-buffer fix.

## In This Build

- Modern Vita controls, a Classic option and Vita button prompts.
- Seven camera zoom levels, with Auto Zoom and two extra close-up views.
- GPU skinning, renderer optimizations and a packaged startup shader cache.
- Recent fixes for startup delays, loading-screen flicker and stadium-preview
  audio timing.
- Smoother movie pacing without dropping the audio when a picture is skipped.

## Install

On an **Apple Silicon Mac**, the simplest route uses
[CBOC Vita Installer](https://github.com/CBOC-Vita-Lab/vita-installer/releases):

1. Connect your Vita, then select the matching release ZIP or its extracted folder.
2. Select your own **Wii USA Rev 1 (`R4QE01`)** ISO/RVZ or complete game extraction.
3. Add any missing required file when prompted, choose your artwork, and install.

[Desktop Install](DESKTOP-INSTALL.md) lists the requirements. No separate Python,
Dolphin or compiling is needed for that route. Windows and Intel Mac users can
follow [Install](INSTALL.md): install the VPK with VitaShell and copy your own
game folders. Desktop builds for those platforms are not ready yet.

[Start Here](START-HERE.txt) is the short checklist. [Install](INSTALL.md)
has the requirements, folder map and update steps. The VPK already carries
its startup shaders; no manual `eboot.bin` or shader replacement is needed.

Use `Strikers-movie-fix-20261019.vpk` or its matching install ZIP. The complete
files are also in this repository. The executable matches the approved update
installed on a physical Vita; the full desktop installation still needs its
own hardware check. This remains a private release, not the public launch.
Game data and the proprietary shader compiler are not included. [Known Issues](KNOWN-ISSUES.md) and
[Source Record](SOURCE.md) hold the testing and release details.

[Controls](CONTROLS.md) | [Building](BUILDING.md) | [Source Record](SOURCE.md)
| [Technical Notes](TECHNICAL.md) | [Included Files](ASSETS.md)
| [Licenses](THIRD_PARTY_NOTICES.md) | [Changes](CHANGELOG.md) | [Credits](CREDITS.md)

Upstream contributors retain their credit and rights; see [Credits](CREDITS.md).
