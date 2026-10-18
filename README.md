# Mario Strikers Charged Vita

A native PS Vita port of Mario Strikers Charged, based on the reconstructed
Wii source. It runs the game as ARM code, with a Vita graphics, audio and
controller layer.

## In This Build

- Modern Vita controls, a Classic option and Vita button prompts.
- Seven camera zoom levels, with Auto Zoom and two extra close-up views.
- GPU skinning, renderer optimizations and a packaged startup shader cache.
- Recent fixes for startup delays, loading-screen flicker and stadium-preview
  audio timing.

## Install

1. [Prepare your game](PREPARE-GAME.md) with the included helper. It downloads
   the verified extractor and checks your **Wii USA Rev 1 (`R4QE01`)** dump.
2. Copy the prepared `sys/` and `files/` into `ux0:data/mscharged/game/`.
3. Install the matching player VPK with VitaShell and open the game.

[Start Here](START-HERE.txt) is the short checklist. [Install](INSTALL.md)
has the requirements, folder map and update steps. The VPK already carries
its startup shaders; no manual `eboot.bin` or shader replacement is needed.

The `a8d4ad6` package is a private preview, not a public download.
Game data is not included. [Known Issues](KNOWN-ISSUES.md) and
[Source Record](SOURCE.md) hold the testing and release details.

[Controls](CONTROLS.md) | [Building](BUILDING.md) | [Source Record](SOURCE.md)
| [Technical Notes](TECHNICAL.md) | [Included Files](ASSETS.md)
| [Licenses](THIRD_PARTY_NOTICES.md) | [Changes](CHANGELOG.md) | [Credits](CREDITS.md)

CBOC, Claude and ChatGPT contributed to the port and release preparation;
upstream contributors retain their credit and rights.
